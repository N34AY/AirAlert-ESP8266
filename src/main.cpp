// AirAlert-ESP8266 — main wiring. Boot order per SPEC 25 (Invariant 3),
// non-blocking tick pipeline per SPEC 116.
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

#include "airalert/AlertEngine.h"
#include "airalert/ApiHealth.h"
#include "airalert/BackoffPolicy.h"
#include "airalert/Config.h"
#include "airalert/NotificationEngine.h"
#include "airalert/PreexistingAlerts.h"
#include "airalert/SnapshotBuilder.h"
#include "alerts/AlertsClient.h"
#include "alerts/LocationCatalog.h"
#include "config/ConfigStore.h"
#include "config/SecretsStore.h"
#include "hardware/ButtonController.h"
#include "hardware/RelayController.h"
#include "hardware/Indication.h"
#include "network/WifiService.h"
#include "storage/EventLogStore.h"
#include "web/WebUi.h"

using namespace airalert;

namespace pins {
constexpr uint8_t SIREN_START = 14; // D5 — relay 1, pulse = START button (SPEC 61, 64)
constexpr uint8_t SIREN_STOP = 12;  // D6 — relay 2, pulse = STOP button
constexpr uint8_t MUTE = 5;         // D1 (D6 is taken by the STOP relay)
constexpr uint8_t TEST = 13;        // D7
constexpr uint8_t BUILTIN_LED = 2;  // D4, inverted — SYSTEM indicator
constexpr uint8_t ALERT_OUT = 16;   // D0 — alert LED or indication relay
} // namespace pins

static SecretsStore secrets;
static AppConfig appCfg;
static ConfigStore configStore;
static EventLogStore eventLog;
static AlertsClient client;
static AlertEngine engine;
static SnapshotBuilder builder;
static ApiHealth health;
static BackoffPolicy backoff;
static NotificationEngine notify;
static RelayController relay;
static ButtonController btnMute, btnTest;
static Indication led;
static WifiService wifi;
static WebUi web;
static LocationCatalog catalog;

static uint32_t nextPollAt = 0;

// Per-location threat details (dashboard, SPEC 71).
// Updated only on successful polls, so it always matches the engine state.
static ActiveLocationView activeView;

static void fillActiveView() {
    activeView.locCount = static_cast<uint8_t>(builder.selectedCount());
    for (size_t li = 0; li < builder.selectedCount(); ++li) {
        activeView.uid[li] = builder.selectedUid(li);
        uint8_t n = 0;
        for (uint8_t t = 0; t < kAlertTypeCount; ++t) {
            const auto& cell = builder.locCell(li, static_cast<AlertType>(t));
            if (cell.coverage == Coverage::None) continue;
            // named assignment, NOT positional braces: the field order already
            // bit us once (coverage landed in .type and painted urban fights)
            auto& th = activeView.threats[li][n++];
            th.startedAt = cell.startedAt;
            th.type = t;
            th.coverage = static_cast<uint8_t>(cell.coverage);
        }
        activeView.threatCount[li] = n;
    }
}
static bool ntpStarted = false;
static bool ntpSynced = false;
static bool wasOnline = false;
static bool otaInProgress = false;
static int lastApiHttpCode = 0;
static String lastApiError;
static uint8_t auth401Streak = 0; // consecutive 401s, see doPoll()
static constexpr uint8_t kBadKeyStreak = 15; // P(15 rate-limit 401s in a row) < 1e-4 at any cadence

static bool apiReady() {
    return wifi.online() && ntpSynced && client.hasToken() && appCfg.selectedCount > 0;
}

static void muteNow(bool longPress) {
    relay.forceOff("mute"); // Invariant 4: relay OFF before anything else
    if (longPress) notify.muteLong(millis());
    else notify.muteShort(millis());
    eventLog.log(LogEvent::Mute, longPress ? "scope=snooze" : "scope=until_clear");
}

// Invariant 8 stops everything for OTA - but nothing guaranteed OTA ever ends:
// ESP8266WebServer spins in _uploadReadByte() while a half-open client keeps the
// socket open and sends nothing, so loop() never runs again and the siren stays
// dead with no watchdog (yield() keeps feeding it). A SYS-context deadline runs
// even while loop() is blocked, exactly like the relay safety timer. Rebooting
// is safe: an unfinished Update only touched the staging area, never the
// running sketch, so the device comes back on the old firmware and sirens again.
static constexpr uint32_t kOtaDeadlineMs = 300000; // 5 min
static Ticker otaWatchdog;

static void prepareOta() { // Invariant 8
    otaInProgress = true;
    notify.stopAll();
    relay.forceOff("ota");
    otaWatchdog.once_ms(kOtaDeadlineMs, [] { ESP.restart(); });
}

static void finishFailedOta() {
    otaWatchdog.detach();
    relay.forceOff("ota");
    otaInProgress = false;
}

static void refreshLocations() {
    activeView = ActiveLocationView{}; // drop rows for locations no longer selected
    relay.forceOff("locations");
    notify.resetAlertState();
    engine.reset();
    builder.reset();
    client.invalidateCache();
    lastApiHttpCode = 0;
    lastApiError = "";
    nextPollAt = millis();
}

// Config -> subsystems (SPEC 25 step 4; re-applied after Web UI edits)
static void applyConfig() {
    engine.setConfig({appCfg.startConfirmations, appCfg.endConfirmations,
                      appCfg.partialActive});
    notify.setMuteConfig({appCfg.snoozeMinutes * 60000u, appCfg.muteAllAlertTypes});
    notify.setConfig({appCfg.notifyEscalation, appCfg.notifyAdditionalLocation,
                      appCfg.remindersWhenStale,
                      // START and STOP presses must be audibly separate clicks
                      std::max<uint16_t>(appCfg.relayTestMs, 2 * RelayController::kPulseMs)});
    for (uint8_t i = 0; i < kAlertTypeCount; ++i)
        notify.setProfile(static_cast<AlertType>(i), appCfg.profiles[i]);
    relay.begin(pins::SIREN_START, pins::SIREN_STOP, appCfg.relayActiveHigh,
                appCfg.relayMaxOnMs);
    backoff.setConfig({appCfg.pollIntervalSec * 1000u, 120000, 60000, 300000, 10});
    health.setConfig({appCfg.apiStaleAfterSec * 1000u});
    builder.setSelected(appCfg.selected, appCfg.selectedCount);
    builder.setCatalog(&catalog);
    for (uint8_t i = 0; i < appCfg.selectedCount; ++i) { // catalogue regenerated -> stale uids
        Location known;
        if (!catalog.findByUid(appCfg.selected[i].uid, known))
            Serial.printf("[CFG] location uid=%u not in catalogue - re-select it in the Web UI\n",
                          appCfg.selected[i].uid);
    }
    led.begin(pins::BUILTIN_LED, true, pins::ALERT_OUT,
              appCfg.alertIndicatorActiveHigh, appCfg.alertIndicatorSteady);
}

static const char* eventName(AlertEvent e) {
    switch (e) {
        case AlertEvent::Started: return "ALERT_START";
        case AlertEvent::Escalated: return "ALERT_ESCALATED";
        case AlertEvent::CoverageReduced: return "ALERT_COVERAGE_REDUCED";
        case AlertEvent::LocationAdded: return "ALERT_LOCATION_ADDED";
        case AlertEvent::Ended: return "ALERT_END";
        default: return "?";
    }
}

static PreexistingAlerts preexisting; // SPEC 26

// Shared by the real poller and the dev `sim` command.
static void applyAndNotify(const AlertEngine::Snapshot& snap, bool simulated) {
    preexisting.observe(snap, !engine.synced(), appCfg.partialActive);

    EngineEvent ev[8];
    const size_t n = engine.applySnapshot(snap, ev, 8); // SPEC 168
    for (size_t i = 0; i < n; ++i) {
        const bool alreadyActive =
            ev[i].kind == AlertEvent::Started && preexisting.consumeStart(ev[i].type);
        Serial.printf("[ALARM] %s type=%s%s%s\n", eventName(ev[i].kind),
                      alertTypeToString(ev[i].type),
                      alreadyActive ? " (already active, silent)" : "",
                      simulated ? " (SIM)" : "");
        auto mode = alreadyActive ? NotificationEngine::StartupMode::Silent
                                  : NotificationEngine::StartupMode::Normal;
        // SPEC 20: partial-only alert with siren disabled for partial
        if (ev[i].kind == AlertEvent::Started && !appCfg.partialSiren &&
            engine.status(ev[i].type).coverage == Coverage::Partial)
            mode = NotificationEngine::StartupMode::Silent;
        // One physical air raid, two colours: a colour ending while the other
        // colour is still active is a level change, not an all-clear.
        if (ev[i].kind == AlertEvent::Ended) {
            const AlertType other =
                ev[i].type == AlertType::AirRaid ? AlertType::AirRaidYellow
                : ev[i].type == AlertType::AirRaidYellow ? AlertType::AirRaid : AlertType::Unknown;
            if (other != AlertType::Unknown && engine.status(other).state == AlertState::Active)
                mode = NotificationEngine::StartupMode::Silent;
        }
        notify.onEngineEvent(ev[i], mode, millis());
        switch (ev[i].kind) {
            case AlertEvent::Started:
                eventLog.log(engine.status(ev[i].type).coverage == Coverage::Full
                                 ? LogEvent::AlertFull : LogEvent::AlertPartial,
                             alertTypeToString(ev[i].type));
                break;
            case AlertEvent::Ended:
                eventLog.log(LogEvent::AlertEnd, alertTypeToString(ev[i].type));
                break;
            default: break;
        }
    }
}

static void doPoll() {
    using Kind = AlertsClient::Result::Kind;
    const uint32_t t0 = millis();
    AlertsClient::Result res = client.poll(builder);
    const uint32_t latency = millis() - t0;
    lastApiHttpCode = res.httpCode;
    lastApiError = "";

    BackoffPolicy::Outcome oc;
    switch (res.kind) {
        case Kind::Ok:
            auth401Streak = 0;
            if (!health.online()) eventLog.log(LogEvent::ApiOnline);
            health.onContact(millis());
            oc = BackoffPolicy::Outcome::Success;
            fillActiveView();
            applyAndNotify(builder.snapshot(), false);
            Serial.printf("[API] 200 ok alerts=%u skipped=%u active=%d latency=%lums heap=%u\n",
                          res.stats.total, res.stats.skipped, engine.anyActive(),
                          (unsigned long)latency, ESP.getFreeHeap());
            break;
        case Kind::NotModified:
            auth401Streak = 0;
            if (!health.online()) eventLog.log(LogEvent::ApiOnline);
            health.onContact(millis());
            oc = BackoffPolicy::Outcome::NotModified;
            // A validated cached snapshot is still a confirmation sample.
            applyAndNotify(builder.snapshot(), false);
            Serial.printf("[API] 304 not modified latency=%lums heap=%u\n",
                          (unsigned long)latency, ESP.getFreeHeap());
            break;
        case Kind::AuthError:
            // ukrainealarm.com rate-limits with a bare 401 identical to a bad
            // key: ~3 accepted requests per key per minute, the rest 401 - about
            // a third of polls at 20 s (docs/RESEARCH.md). So one 401 is "poll
            // again at the normal cadence": no health failure, no ladder. Only
            // a long streak means the key really is bad; then the SPEC 163
            // 5-min backoff applies, journaled once per streak.
            if (auth401Streak < 255) ++auth401Streak;
            if (auth401Streak < kBadKeyStreak) {
                lastApiError = "rate_limited";
                oc = BackoffPolicy::Outcome::Success;
            } else {
                lastApiError = "unauthorized";
                health.onFailure();
                oc = BackoffPolicy::Outcome::AuthError;
                if (auth401Streak == kBadKeyStreak) eventLog.log(LogEvent::Api401);
            }
            break;
        case Kind::Forbidden:
            lastApiError = "forbidden";
            health.onFailure();
            oc = BackoffPolicy::Outcome::AuthError;
            eventLog.log(LogEvent::Api403);
            break;
        case Kind::RateLimited:
            lastApiError = "rate_limited";
            health.onFailure();
            oc = BackoffPolicy::Outcome::RateLimited;
            eventLog.log(LogEvent::Api429);
            break;
        case Kind::ParseError: {
            lastApiError = "parse_error";
            health.onFailure(); // snapshot NOT applied - Invariant 6
            oc = BackoffPolicy::Outcome::ParseError;
            char detail[64];
            snprintf(detail, sizeof detail, "%s heap=%u",
                     res.parseDetail ? res.parseDetail : "?", res.heapAtError);
            eventLog.log(LogEvent::ApiParseError, detail); // serial + journal
            break;
        }
        default:
            lastApiError = res.httpCode < 0 ? "network_error" : "http_error";
            if (health.online()) eventLog.log(LogEvent::ApiOffline);
            health.onFailure();
            oc = BackoffPolicy::Outcome::NetError;
            Serial.printf("[API] API_OFFLINE code=%d heap=%u\n", res.httpCode, ESP.getFreeHeap());
            break;
    }
    nextPollAt = millis() +
                 backoff.next(oc, res.retryAfterSec, static_cast<uint8_t>(ESP.random() & 0xFF));
}

// ---- serial console ---------------------------------------------------------
static void handleSerialLine(String line) {
    line.trim();
    if (line.startsWith("setwifi ")) {
        const int sp = line.indexOf(' ', 8);
        if (sp < 0) { Serial.println("[CFG] usage: setwifi <ssid> <pass>"); return; }
        SecretsStore next = secrets;
        next.wifiSsid = line.substring(8, sp);
        next.wifiPass = line.substring(sp + 1);
        if (!next.wifiSsid.length() || next.wifiSsid.length() > 32 ||
            next.wifiPass.length() > 64 || !next.save()) {
            Serial.println("[CFG] invalid Wi-Fi credentials or SAVE FAILED");
            return;
        }
        secrets = next;
        Serial.printf("[CFG] wifi ssid='%s' saved, restarting\n", secrets.wifiSsid.c_str());
        delay(500);
        ESP.restart();
    } else if (line.startsWith("settoken ")) {
        SecretsStore next = secrets;
        next.apiToken = line.substring(9);
        if (next.apiToken.length() < 10 || next.apiToken.length() > 256 || !next.save()) {
            Serial.println("[CFG] invalid token or SAVE FAILED");
            return;
        }
        secrets = next;
        Serial.println("[CFG] token saved, restarting");
        delay(500);
        ESP.restart();
    } else if (line.startsWith("setpass ")) {
        const String password = line.substring(8);
        if (password.length() < 6 || password.length() > 128) {
            Serial.println("[CFG] web password must be 6..128 characters");
            return;
        }
        SecretsStore next = secrets;
        next.setWebPassword(password);
        if (!next.save()) {
            Serial.println("[CFG] web password SAVE FAILED");
            return;
        }
        secrets = next;
        Serial.println("[CFG] web password saved");
    } else if (line.startsWith("setmock")) { // dev: emulator URL, empty = real API
#ifdef AIRALERT_DEV
        String mockUrl = line.length() > 8 ? line.substring(8) : "";
        mockUrl.trim();
        if (mockUrl.length() > 192 ||
            (mockUrl.length() && !mockUrl.startsWith("http://") &&
             !mockUrl.startsWith("https://"))) {
            Serial.println("[CFG] usage: setmock [http(s)://host:port/path]");
            return;
        }
        SecretsStore next = secrets;
        next.mockUrl = mockUrl;
        if (!next.save()) {
            Serial.println("[CFG] mock URL SAVE FAILED");
            return;
        }
        secrets = next;
        Serial.printf("[CFG] mock url %s, restarting\n",
                      secrets.mockUrl.length() ? "set" : "cleared");
        delay(300);
        ESP.restart();
#else
        Serial.println("[CFG] dev build only");
#endif
    } else if (line == "forgetwifi") { // SPEC 82
        if (wifi.forget())
            eventLog.log(LogEvent::ConfigChanged, "wifi_forget");
        else
            Serial.println("[CFG] Wi-Fi credentials SAVE FAILED");
    } else if (line == "show") { // secrets never printed (SPEC 6)
        Serial.printf("[CFG] ssid='%s' token: %s webpass: %s\n", secrets.wifiSsid.c_str(),
                      secrets.apiToken.length() ? "configured" : "MISSING",
                      secrets.hasWebPassword() ? "set" : "NOT SET");
    } else if (line == "restart") {
        ESP.restart();
    } else if (line == "mute") {
        muteNow(false);
    } else if (line == "unmute") {
        notify.unmute();
        eventLog.log(LogEvent::Unmute);
    } else if (line == "test") { // SPEC 54: full START -> STOP cycle
        notify.manualTest(millis());
        eventLog.log(LogEvent::Test, "source=serial");
    } else if (line == "test start" || line == "test stop") { // one relay
        if (notify.playing()) { Serial.println("[RELAY] busy: a pattern is playing"); return; }
        const bool start = line == "test start";
        relay.testPulse(start);
        eventLog.log(LogEvent::Test, start ? "source=serial relay=start" : "source=serial relay=stop");
    } else if (line == "status") {
        Serial.printf("[ST] wifi=%d ntp=%d active=%d muted=%d playing=%d relay=%d tripped=%d heap=%u\n",
                      wifi.online(), ntpSynced, engine.anyActive(), notify.muted(),
                      notify.playing(), relay.isOn(), relay.safetyTripped(), ESP.getFreeHeap());
    } else if (line == "config") {
        JsonDocument d;
        configToJson(appCfg, d); // never contains secrets (SPEC 91)
        serializeJson(d, Serial);
        Serial.println();
    } else if (line == "log") {
        for (const char* path : {eventLog.olderPath(), eventLog.activePath()}) {
            File f = LittleFS.open(path, "r");
            if (!f) continue;
            while (f.available()) Serial.write(f.read());
            f.close();
        }
    } else if (line.startsWith("sim ")) {
#ifdef AIRALERT_DEV
        AlertEngine::Snapshot snap{};
        const int sp = line.indexOf(' ', 4);
        const String typeStr = sp > 0 ? line.substring(4, sp) : line.substring(4);
        const String covStr = sp > 0 ? line.substring(sp + 1) : "none";
        Coverage cov = covStr == "full" ? Coverage::Full
                       : covStr == "partial" ? Coverage::Partial : Coverage::None;
        snap.types[static_cast<uint8_t>(alertTypeFromString(typeStr.c_str()))] =
            {cov, static_cast<uint8_t>(cov != Coverage::None), 1755763200};
        applyAndNotify(snap, true);
        nextPollAt = millis() + 300000; // hold real polls off while simulating
        Serial.printf("[SIM] applied %s=%s\n", typeStr.c_str(), covStr.c_str());
#else
        Serial.println("[SIM] dev build only");
#endif
    } else if (line.length()) {
        Serial.println("[CFG] setwifi|settoken|setpass|setmock|forgetwifi|show|restart|mute|unmute|test [start|stop]|status|config|log|sim");
    }
}

static void pollSerial() {
    static String buf;
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (buf.length()) handleSerialLine(buf);
            buf = "";
        } else if (buf.length() < 160) {
            buf += c;
        }
    }
}

// ---- boot -------------------------------------------------------------------
void setup() {
    // step 1: relay safe OFF (Invariant 3). The relay pins are left as reset
    // inputs (high-Z = released) until applyConfig() knows the real polarity:
    // driving them with a guessed one would press START+STOP together on an
    // active-LOW module for as long as LittleFS takes to mount.

    Serial.begin(115200);
    Serial.println();
    Serial.printf("[BOOT] AirAlert-ESP8266 %s (%s)\n", AIRALERT_VERSION, __DATE__);
    Serial.printf("[BOOT] reset reason: %s\n", ESP.getResetReason().c_str());

    btnMute.begin(pins::MUTE);
    btnTest.begin(pins::TEST);
    // config not loaded yet: safe defaults, re-begun in applyConfig()
    led.begin(pins::BUILTIN_LED, true, pins::ALERT_OUT, true, false);

    if (!LittleFS.begin()) Serial.println("[FS] LittleFS mount failed");

    if (!configStore.load(appCfg)) {
        appCfg = AppConfig{}; // corrupt/missing -> defaults (SPEC 128)
        Serial.println("[CFG] using default config");
    }
    applyConfig();
    eventLog.begin();
    eventLog.log(LogEvent::Boot, ESP.getResetReason().c_str());

    if (!secrets.load())
        Serial.println("[CFG] secrets missing or invalid; using setup defaults");
    client.begin(secrets.apiToken);
#ifdef AIRALERT_DEV
    String mockUrl = secrets.mockUrl;
#ifdef AIRALERT_MOCK_URL
    if (!mockUrl.length()) mockUrl = AIRALERT_MOCK_URL;
#endif
    if (mockUrl.length()) {
        client.setMockUrl(mockUrl);
        Serial.printf("[API] MOCK MODE: %s\n", mockUrl.c_str());
    }
#endif
    wifi.begin(&secrets); // STA or provisioning AP (SPEC 78-81)

    web.begin({&appCfg, &engine, &notify, &health, &relay, &configStore, &secrets,
               &eventLog, &wifi, &activeView,
               [] { applyConfig(); },
               [](const String& t) {
                   client.begin(t);
                   auth401Streak = 0;     // new key: judge it afresh
                   nextPollAt = millis(); // poll with the new token immediately
               },
               [] { refreshLocations(); },
               [](bool longPress) { muteNow(longPress); },
               [] {
                   notify.unmute();
                   eventLog.log(LogEvent::Unmute);
               },
               [] { prepareOta(); },
               [] { finishFailedOta(); },
               &ntpSynced, &lastApiHttpCode, &lastApiError});

    if (!secrets.apiToken.length())
        Serial.println("[API] DEVICE NOT READY: no API token (SPEC 162)");
}

// Physical START/STOP presses -> event log, so the Web UI journal shows them.
static void logRelayPresses() {
    RelayController::Press p;
    uint8_t dropped;
    char detail[48];
    while (relay.popPress(p, dropped)) {
        snprintf(detail, sizeof detail, "relay=%s reason=%s", p.start ? "start" : "stop", p.why);
        eventLog.log(LogEvent::RelayPress, detail);
    }
    if (dropped) {
        snprintf(detail, sizeof detail, "dropped=%u", dropped);
        eventLog.log(LogEvent::RelayPress, detail);
    }
}

static void updateLed(bool sirenOn) {
    // SYSTEM channel: solid = fully operational
    Indication::SysState sys;
    if (wifi.state() == WifiService::State::Provisioning ||
        !secrets.apiToken.length() || otaInProgress)
        sys = Indication::SysState::Setup;
    else if (wifi.online() && ntpSynced && health.online() && !health.stale(millis()))
        sys = Indication::SysState::Ok;
    else
        sys = Indication::SysState::Degraded;
    led.setSystem(sys);

    // ALERT channel
    auto view = Indication::AlertView::None;
    if (engine.anyActive()) {
        bool full = false;
        for (uint8_t i = 0; i < kAlertTypeCount; ++i)
            if (engine.status(static_cast<AlertType>(i)).coverage == Coverage::Full)
                full = true;
        if (full) view = Indication::AlertView::Full;
        else if (appCfg.partialLed) view = Indication::AlertView::Partial; // SPEC 20
    }
    led.setAlert(view, notify.muted(), sirenOn);
}

// SPEC 116: non-blocking tick pipeline
void loop() {
    const uint32_t now = millis();
    pollSerial();
    wifi.tick(now);
    web.tick();
    if (otaInProgress) return; // Invariant 8: nothing else runs during OTA

    // Wi-Fi online/offline transitions -> log + NTP kick-off
    if (wifi.online() != wasOnline) {
        wasOnline = wifi.online();
        eventLog.log(wasOnline ? LogEvent::WifiConnected : LogEvent::WifiDisconnected);
        if (wasOnline && !ntpStarted) {
            configTime("UTC0", "pool.ntp.org", "time.google.com"); // SPEC 83
            ntpStarted = true;
        }
    }
    if (ntpStarted && !ntpSynced && time(nullptr) > 1600000000) {
        ntpSynced = true;
        Serial.printf("[NTP] time ok: %lld\n", static_cast<long long>(time(nullptr)));
    }

    switch (btnMute.tick(now)) {
        case ButtonController::Event::LongPress: muteNow(true); break;
        case ButtonController::Event::Release:
            if (btnMute.wasShortPress()) muteNow(false);
            break;
        default: break;
    }
    if (btnTest.tick(now) == ButtonController::Event::LongPress) { // SPEC 54
        notify.manualTest(now);
        eventLog.log(LogEvent::Test, "source=button");
    }

    notify.setApiStale(health.stale(now));
    const bool siren = notify.tick(now);
    relay.tick(siren, now);
    if (relay.consumeSafetyTrip())
        eventLog.log(LogEvent::RelaySafetyTrip); // Invariant 2 fired
    logRelayPresses();

    updateLed(relay.isOn());
    led.tick(now);

    // The poll is synchronous (TLS handshake + body: seconds). Started while
    // the relay is ON it would stretch the siren's ON phase to the poll
    // length; waiting for the pattern's OFF gap stretches silence instead.
    // ON phases are bounded by RelayGuard, so the wait is bounded too.
    if (apiReady() && !relay.isOn() && static_cast<int32_t>(now - nextPollAt) >= 0) doPoll();

    static uint32_t lastBeat = 0;
    if (now - lastBeat >= 30000) {
        lastBeat = now;
        Serial.printf("[SYS] uptime=%lus heap=%u wifi=%d active=%d stale=%d online=%d relay=%d muted=%d\n",
                      (unsigned long)(now / 1000), ESP.getFreeHeap(), wifi.online(),
                      engine.anyActive(), health.stale(now), health.online(),
                      relay.isOn(), notify.muted());
    }
}
