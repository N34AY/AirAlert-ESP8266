#include "WebUi.h"
#include <LittleFS.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <WiFiClientSecureBearSSL.h>
#include <bearssl/bearssl_hash.h>
#include <osapi.h>
#include "alerts/CaBundle.h"
#include "WebAssets.h"
#include "airalert/SnapshotBuilder.h"

using namespace airalert;

static const char* coverageName(Coverage c) {
    return c == Coverage::Full ? "full" : c == Coverage::Partial ? "partial" : "none";
}

static bool constantTimeEqual(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    uint8_t different = 0;
    for (size_t i = 0; i < a.length(); ++i)
        different |= static_cast<uint8_t>(a[i] ^ b[i]);
    return different == 0;
}

static bool sameLocations(const AppConfig& a, const AppConfig& b) {
    if (a.selectedCount != b.selectedCount) return false;
    for (uint8_t i = 0; i < a.selectedCount; ++i) {
        const Location& left = a.selected[i];
        const Location& right = b.selected[i];
        if (left.uid != right.uid || left.type != right.type ||
            left.oblastUid != right.oblastUid || left.raionUid != right.raionUid)
            return false;
    }
    return true;
}

static bool isSha256Hex(const String& value) {
    if (value.length() != 64) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

void WebUi::begin(const Deps& d) {
    d_ = d;
    server_.collectHeaders("X-Auth"); // core 3.x variadic form

    server_.on("/", HTTP_GET, [this] {
        server_.sendHeader("Content-Encoding", "gzip");
        server_.send_P(200, ASSET_INDEX_HTML_MIME,
                       reinterpret_cast<const char*>(ASSET_INDEX_HTML), ASSET_INDEX_HTML_LEN);
    });
    server_.on("/locations.json", HTTP_GET, [this] {
        server_.sendHeader("Content-Encoding", "gzip");
        server_.sendHeader("Cache-Control", "max-age=86400");
        server_.send_P(200, ASSET_LOCATIONS_JSON_MIME,
                       reinterpret_cast<const char*>(ASSET_LOCATIONS_JSON), ASSET_LOCATIONS_JSON_LEN);
    });

    // Captive portal (SPEC 80): OS probes redirect to the setup page while
    // in provisioning mode; otherwise they 404.
    auto captive = [this] {
        if (d_.wifi->state() == WifiService::State::Provisioning) {
            server_.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup");
            server_.send(302, "text/plain", "");
        } else {
            sendError(404, "NOT_FOUND", "not provisioning");
        }
    };
    for (const char* probe : {"/generate_204", "/gen_204", "/hotspot-detect.html",
                              "/ncsi.txt", "/connecttest.txt", "/fwlink", "/redirect"})
        server_.on(probe, HTTP_GET, captive);
    server_.on("/setup", HTTP_GET, [this] {
        server_.sendHeader("Content-Encoding", "gzip");
        server_.send_P(200, ASSET_SETUP_HTML_MIME,
                       reinterpret_cast<const char*>(ASSET_SETUP_HTML), ASSET_SETUP_HTML_LEN);
    });
    server_.on("/api/v1/scan", HTTP_GET, [this] { handleScan(); });
    server_.on("/api/v1/setup", HTTP_POST, [this] { handleSetup(); });
    server_.on("/api/v1/wifi/forget", HTTP_POST, [this] { // SPEC 82
        if (!authed()) return;
        if (!d_.wifi->forget()) {
            sendError(500, "FS_ERROR", "cannot clear Wi-Fi credentials");
            return;
        }
        d_.log->log(LogEvent::ConfigChanged, "wifi_forget");
        server_.send(200, "application/json", "{\"ok\":true}");
    });
    server_.on("/api/v1/ota/upload", HTTP_POST,
               [this] { // final response after upload completes
                   if (!otaUploadAuthorized_) return;
                   if (!otaUploadSucceeded_ || Update.hasError()) {
                       d_.log->log(LogEvent::OtaFailed);
                       const String error = Update.hasError()
                                                ? Update.getErrorString()
                                                : String("upload incomplete");
                       Update.end();
                       d_.finishFailedOta();
                       sendError(500, "OTA_FAILED", error.c_str());
                   } else {
                       d_.log->log(LogEvent::OtaSuccess);
                       server_.send(200, "application/json", "{\"ok\":true}");
                       rebootAt_ = millis() + 500;
                   }
               },
               [this] { handleOtaUpload(); });
    server_.on("/api/v1/ota/url", HTTP_POST, [this] { handleOtaUrl(); });

    server_.on("/api/v1/login", HTTP_POST, [this] { handleLogin(); });
    server_.on("/api/v1/status", HTTP_GET, [this] { handleStatus(); });
    server_.on("/api/v1/config", HTTP_GET, [this] { handleConfigGet(); });
    server_.on("/api/v1/config", HTTP_PUT, [this] { handleConfigPut(); });
    server_.on("/api/v1/locations", HTTP_PUT, [this] { handleLocationsPut(); });
    server_.on("/api/v1/secrets/api-token", HTTP_PUT, [this] { handleTokenPut(); });
    server_.on("/api/v1/events", HTTP_GET, [this] { handleEvents(); });
    server_.on("/api/v1/system", HTTP_GET, [this] { handleSystem(); });

    server_.on("/api/v1/mute", HTTP_POST, [this] {
        if (!authed()) return;
        d_.mute(false);
        server_.send(200, "application/json", "{\"ok\":true}");
    });
    server_.on("/api/v1/unmute", HTTP_POST, [this] {
        if (!authed()) return;
        d_.unmute();
        server_.send(200, "application/json", "{\"ok\":true}");
    });
    server_.on("/api/v1/test/relay", HTTP_POST, [this] { // SPEC 54, 77
        if (!authed()) return;
        d_.notify->manualTest(millis());
        d_.log->log(LogEvent::Test, "source=web");
        server_.send(200, "application/json", "{\"ok\":true}");
    });
    server_.on("/api/v1/relay/off", HTTP_POST, [this] { // SPEC 125 manual override
        if (!authed()) return;
        d_.notify->stopAll();
        d_.relay->forceOff();
        server_.send(200, "application/json", "{\"ok\":true}");
    });
    server_.on("/api/v1/reboot", HTTP_POST, [this] {
        if (!authed()) return;
        server_.send(200, "application/json", "{\"ok\":true}");
        rebootAt_ = millis() + 300;
    });

    server_.onNotFound([this] { sendError(404, "NOT_FOUND", "unknown endpoint"); });
    server_.begin();
}

void WebUi::tick() {
    server_.handleClient();
    if (rebootAt_ && static_cast<int32_t>(millis() - rebootAt_) >= 0) ESP.restart();
}

// ---- auth -------------------------------------------------------------------

bool WebUi::authed() {
    const String tok = server_.header("X-Auth");
    if (tok.length() == 32) {
        for (auto& s : sessions_) {
            if (s.token[0] && tok == s.token &&
                static_cast<int32_t>(millis() - s.expiresAt) < 0)
                return true;
        }
    }
    sendError(401, "UNAUTHORIZED", "login required");
    return false;
}

void WebUi::handleLogin() {
    if (static_cast<int32_t>(millis() - lockUntil_) < 0) { // SPEC 100 rate limit
        sendError(429, "LOCKED", "too many attempts");
        return;
    }
    JsonDocument d;
    if (deserializeJson(d, server_.arg("plain")) != DeserializationError::Ok ||
        !d_.secrets->hasWebPassword() ||
        !d_.secrets->checkWebPassword(d["password"] | "")) {
        if (++loginFails_ >= 5) {
            loginFails_ = 0;
            lockUntil_ = millis() + 60000;
        }
        sendError(401, "BAD_CREDENTIALS",
                  d_.secrets->hasWebPassword() ? "invalid password"
                                               : "password not set (serial: setpass)");
        return;
    }
    loginFails_ = 0;
    // oldest slot gets recycled
    Session* slot = &sessions_[0];
    for (auto& s : sessions_)
        if (static_cast<int32_t>(s.expiresAt - slot->expiresAt) < 0) slot = &s;
    uint8_t randomToken[16];
    os_get_random(randomToken, sizeof randomToken);
    for (uint8_t i = 0; i < sizeof randomToken; ++i)
        snprintf(slot->token + i * 2, 3, "%02x", randomToken[i]);
    slot->expiresAt = millis() + kSessionTtlMs;
    JsonDocument out;
    out["token"] = slot->token;
    sendJson(out);
}

// ---- handlers ---------------------------------------------------------------

void WebUi::handleStatus() { // SPEC 127; auth required (SPEC 99 default)
    if (!authed()) return;
    JsonDocument d;
    d["device"] = d_.cfg->deviceName;
    d["firmware"] = AIRALERT_VERSION;
    d["uptime_sec"] = millis() / 1000;
    d["wifi"]["connected"] = WiFi.status() == WL_CONNECTED;
    d["wifi"]["rssi"] = WiFi.RSSI();
    const uint32_t since = d_.health->sinceContactMs(millis());
    d["api"]["online"] = d_.health->online();
    d["api"]["stale"] = d_.health->stale(millis());
    d["api"]["ever_synced"] = d_.engine->synced();
    d["api"]["ntp_synced"] = *d_.ntpSynced;
    d["api"]["selected_locations"] = d_.cfg->selectedCount;
    if (*d_.lastApiHttpCode) d["api"]["last_http_code"] = *d_.lastApiHttpCode;
    else d["api"]["last_http_code"] = nullptr;
    if (d_.lastApiError->length()) d["api"]["last_error"] = *d_.lastApiError;
    else d["api"]["last_error"] = nullptr;
    const char* waitingReason = nullptr;
    if (!d_.engine->synced()) {
        if (d_.cfg->selectedCount == 0) waitingReason = "locations";
        else if (!d_.wifi->online()) waitingReason = "wifi";
        else if (d_.secrets->apiToken.length() == 0) waitingReason = "token";
        else if (!*d_.ntpSynced) waitingReason = "time";
        else if (d_.lastApiError->length()) waitingReason = d_.lastApiError->c_str();
        else waitingReason = "api";
    }
    if (waitingReason) d["api"]["waiting_reason"] = waitingReason;
    else d["api"]["waiting_reason"] = nullptr;
    if (since == UINT32_MAX) d["api"]["last_contact_sec"] = nullptr;
    else d["api"]["last_contact_sec"] = since / 1000;
    d["api"]["token_present"] = d_.secrets->apiToken.length() > 0; // value never sent (SPEC 6)
    JsonArray types = d["alerts"]["types"].to<JsonArray>();
    bool any = false;
    for (uint8_t i = 0; i < kAlertTypeCount; ++i) {
        const auto& st = d_.engine->status(static_cast<AlertType>(i));
        if (st.state != AlertState::Active) continue;
        any = true;
        JsonObject o = types.add<JsonObject>();
        o["type"] = alertTypeToString(static_cast<AlertType>(i));
        o["coverage"] = coverageName(st.coverage);
        o["started_at"] = st.startedAt;
    }
    // Per-location breakdown (SPEC 71): every selected location that has
    // threats, each with its own coverage and start time.
    JsonArray jlocs = d["alerts"]["locations"].to<JsonArray>();
    if (d_.activeView) {
        for (uint8_t li = 0; li < d_.activeView->locCount; ++li) {
            if (!d_.activeView->threatCount[li]) continue;
            JsonObject jl = jlocs.add<JsonObject>();
            jl["uid"] = d_.activeView->uid[li];
            JsonArray jt = jl["threats"].to<JsonArray>();
            for (uint8_t k = 0; k < d_.activeView->threatCount[li]; ++k) {
                const auto& t = d_.activeView->threats[li][k];
                JsonObject o = jt.add<JsonObject>();
                o["type"] = alertTypeToString(static_cast<AlertType>(t.type));
                o["coverage"] = coverageName(static_cast<Coverage>(t.coverage));
                o["started_at"] = t.startedAt;
            }
        }
    }
    d["alerts"]["active"] = any;
    d["alerts"]["muted"] = d_.notify->muted();
    d["alerts"]["count"] = types.size();
    d["relay"]["active"] = d_.relay->isOn();
    d["relay"]["tripped"] = d_.relay->safetyTripped();
    sendJson(d);
}

void WebUi::handleConfigGet() {
    if (!authed()) return;
    JsonDocument d;
    configToJson(*d_.cfg, d); // never contains secrets (SPEC 91)
    sendJson(d);
}

void WebUi::handleConfigPut() {
    if (!authed()) return;
    JsonDocument d;
    if (deserializeJson(d, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "BAD_JSON", "invalid JSON body");
        return;
    }
    AppConfig next = *d_.cfg; // start from current, overlay body (SPEC 90 style)
    const ConfigError err = configFromJson(d.as<JsonVariantConst>(), next);
    if (err != ConfigError::None) {
        sendError(422, "INVALID_CONFIG", "value out of allowed range");
        return;
    }
    const bool locationsChanged = !sameLocations(*d_.cfg, next);
    if (!d_.configStore->save(next)) {
        sendError(500, "FS_ERROR", "cannot persist config");
        return;
    }
    *d_.cfg = next;
    d_.applyConfig();
    if (locationsChanged) d_.refreshAlerts();
    d_.log->log(LogEvent::ConfigChanged, "source=web");
    server_.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleLocationsPut() {
    if (!authed()) return;
    JsonDocument d;
    if (deserializeJson(d, server_.arg("plain")) != DeserializationError::Ok ||
        d["locations"].isNull()) {
        sendError(400, "BAD_JSON", "expected {locations:[...]}");
        return;
    }
    AppConfig next = *d_.cfg;
    JsonDocument wrap; // reuse the config parser for just the locations array
    wrap["locations"] = d["locations"];
    const ConfigError err = configFromJson(wrap.as<JsonVariantConst>(), next);
    if (err != ConfigError::None || next.selectedCount == 0) {
        sendError(422, "INVALID_CONFIG", "bad location list");
        return;
    }
    if (!d_.configStore->save(next)) {
        sendError(500, "FS_ERROR", "cannot persist locations");
        return;
    }
    *d_.cfg = next;
    d_.applyConfig();
    d_.refreshAlerts();
    d_.log->log(LogEvent::ConfigChanged, "locations");
    server_.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleTokenPut() { // write-only (SPEC 91)
    if (!authed()) return;
    JsonDocument d;
    const String tok = (deserializeJson(d, server_.arg("plain")) == DeserializationError::Ok)
                           ? (d["token"] | "") : String();
    if (tok.length() < 10 || tok.length() > 256) {
        sendError(422, "BAD_TOKEN", "token length must be 10..256");
        return;
    }
    SecretsStore next = *d_.secrets;
    next.apiToken = tok;
    if (!next.save()) {
        sendError(500, "FS_ERROR", "cannot persist API token");
        return;
    }
    *d_.secrets = next;
    d_.setApiToken(tok);
    d_.log->log(LogEvent::ConfigChanged, "api_token");
    server_.send(200, "application/json", "{\"ok\":true}");
}

void WebUi::handleEvents() {
    if (!authed()) return;
    if (!d_.log->healthy()) {
        sendError(500, "JOURNAL_ERROR", "event journal storage is unavailable");
        return;
    }
    server_.sendHeader("Cache-Control", "no-store");
    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server_.send(200, "application/x-ndjson", "");
    // Stream oldest -> newest; the UI reverses records for newest-first view.
    for (const char* path : {d_.log->olderPath(), d_.log->activePath()}) {
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        uint8_t buf[256];
        size_t n;
        while ((n = f.read(buf, sizeof buf)) > 0)
            server_.sendContent(reinterpret_cast<const char*>(buf), n);
        f.close();
    }
    server_.sendContent("");
}

void WebUi::handleSystem() { // SPEC 114, 126
    if (!authed()) return;
    JsonDocument d;
    d["firmware"] = AIRALERT_VERSION;
    d["build"] = AIRALERT_BUILD; // set per build in platformio.ini, unlike __DATE__ (per-file compile)
    d["core"] = ESP.getCoreVersion();
    d["reset_reason"] = ESP.getResetReason();
    d["uptime_sec"] = millis() / 1000;
    d["heap_free"] = ESP.getFreeHeap();
    d["heap_max_block"] = ESP.getMaxFreeBlockSize();
    d["heap_frag_pct"] = ESP.getHeapFragmentation();
    d["flash_size"] = ESP.getFlashChipRealSize();
    d["sketch_size"] = ESP.getSketchSize();
    d["sketch_free"] = ESP.getFreeSketchSpace();
    FSInfo fs;
    if (LittleFS.info(fs)) {
        d["fs_used"] = fs.usedBytes;
        d["fs_total"] = fs.totalBytes;
    }
    d["ip"] = WiFi.localIP().toString();
    d["rssi"] = WiFi.RSSI();
    d["catalogue"] = "ukrainealarm /api/v3/regions"; // no count: it would rot
    sendJson(d);
}

void WebUi::handleScan() {
    // Open only on the first-run portal, where no session can exist yet. On a
    // configured device this must require auth: scanNetworks() blocks the loop
    // for ~2 s, so an unauthenticated LAN client could stall polling and the
    // siren patterns by hammering it.
    if (d_.wifi->state() != WifiService::State::Provisioning && !authed()) return;
    const int n = WiFi.scanNetworks();
    JsonDocument d;
    JsonArray arr = d["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["open"] = WiFi.encryptionType(i) == ENC_TYPE_NONE;
    }
    WiFi.scanDelete();
    sendJson(d);
}

void WebUi::handleSetup() { // SPEC 80
    JsonDocument d;
    if (deserializeJson(d, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "BAD_JSON", "invalid JSON body");
        return;
    }
    const String setupKey = d["setup_key"] | "";
    const bool portalAuthorized =
        d_.wifi->state() == WifiService::State::Provisioning &&
        constantTimeEqual(setupKey, d_.wifi->apPass());
    if (!portalAuthorized) {
        if (!d_.secrets->hasWebPassword()) {
            sendError(401, "BAD_SETUP_KEY", "setup key required");
            return;
        }
        if (!authed()) return;
    }

    const String ssid = d["wifi_ssid"] | "";
    const String wifiPass = d["wifi_pass"] | "";
    const String token = d["api_token"] | "";
    const String apass = d["admin_pass"] | "";
    const String name = d["device_name"] | "";
    if (!ssid.length() || ssid.length() > 32) {
        sendError(422, "BAD_SSID", "wifi_ssid length must be 1..32");
        return;
    }
    if (wifiPass.length() > 64) {
        sendError(422, "BAD_WIFI_PASSWORD", "wifi password too long");
        return;
    }
    if (token.length() && (token.length() < 10 || token.length() > 256)) {
        sendError(422, "BAD_TOKEN", "token length must be 10..256");
        return;
    }
    if (!d_.secrets->hasWebPassword() && apass.length() < 6) {
        sendError(422, "BAD_PASSWORD", "admin password >= 6 chars");
        return;
    }
    if (apass.length() > 128 || name.length() >= sizeof d_.cfg->deviceName) {
        sendError(422, "BAD_VALUE", "admin password or device name too long");
        return;
    }

    const AppConfig oldConfig = *d_.cfg;
    AppConfig nextConfig = oldConfig;
    if (name.length()) memcpy(nextConfig.deviceName, name.c_str(), name.length() + 1);
    if (validateConfig(nextConfig) != ConfigError::None ||
        !d_.configStore->save(nextConfig)) {
        sendError(500, "FS_ERROR", "cannot persist device config");
        return;
    }

    SecretsStore nextSecrets = *d_.secrets;
    nextSecrets.wifiSsid = ssid;
    nextSecrets.wifiPass = wifiPass;
    if (token.length()) nextSecrets.apiToken = token;
    if (apass.length() >= 6) nextSecrets.setWebPassword(apass);
    if (!nextSecrets.save()) {
        if (!d_.configStore->save(oldConfig))
            Serial.println("[SETUP] failed to restore previous device config");
        sendError(500, "FS_ERROR", "cannot persist secrets");
        return;
    }

    *d_.cfg = nextConfig;
    *d_.secrets = nextSecrets;
    d_.log->log(LogEvent::ConfigChanged, "setup_portal");
    server_.send(200, "application/json", "{\"ok\":true}");
    rebootAt_ = millis() + 800; // reboot into STA with the new credentials
}

void WebUi::handleOtaUpload() { // SPEC 102, Invariant 8
    HTTPUpload& up = server_.upload();
    if (up.status == UPLOAD_FILE_START) {
        otaUploadAuthorized_ = authed(); // checked once at stream start
        otaUploadStarted_ = false;
        otaUploadSucceeded_ = false;
        if (!otaUploadAuthorized_) return;
        d_.prepareOta();       // relay OFF, queue cleared, patterns stopped
        d_.log->log(LogEvent::OtaStarted, "source=upload");
        const uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        otaUploadStarted_ = Update.begin(maxSketch);
        if (!otaUploadStarted_) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (otaUploadStarted_ && Update.write(up.buf, up.currentSize) != up.currentSize) {
            otaUploadStarted_ = false;
            Update.printError(Serial);
            Update.end();
        }
    } else if (up.status == UPLOAD_FILE_END) {
        otaUploadSucceeded_ = otaUploadStarted_ && Update.end(true);
        if (!otaUploadSucceeded_) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        otaUploadStarted_ = false;
        otaUploadSucceeded_ = false;
        d_.log->log(LogEvent::OtaFailed, "aborted");
        d_.finishFailedOta();
    }
    yield();
}

void WebUi::handleOtaUrl() { // SPEC 103: HTTPS + size + SHA-256 mandatory
    if (!authed()) return;
    JsonDocument d;
    if (deserializeJson(d, server_.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "BAD_JSON", "invalid JSON body");
        return;
    }
    const String url = d["url"] | "";
    String sha = d["sha256"] | "";
    sha.toLowerCase();
    if (!url.startsWith("https://")) { sendError(422, "BAD_URL", "HTTPS required"); return; }
    if (!isSha256Hex(sha)) { sendError(422, "BAD_SHA", "sha256 hex required"); return; }

    d_.prepareOta(); // Invariant 8
    d_.log->log(LogEvent::OtaStarted, "source=url");
    auto fail = [this](int status, const char* code, const String& message,
                       const char* logDetail) {
        d_.log->log(LogEvent::OtaFailed, logDetail);
        d_.finishFailedOta();
        sendError(status, code, message.c_str());
    };

    BearSSL::WiFiClientSecure client;
    static BearSSL::X509List cas(CA_BUNDLE_PEM);
    client.setTrustAnchors(&cas);
    client.setBufferSizes(4096, 512);
    HTTPClient http;
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // GitHub 302s
    if (!http.begin(client, url)) {
        fail(500, "NET", "begin failed", "begin");
        return;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        fail(502, "HTTP_ERROR", String(code), "http");
        return;
    }
    const int len = http.getSize();
    const uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (len <= 0 || static_cast<uint32_t>(len) > maxSketch) {
        http.end();
        fail(422, "BAD_SIZE", "image does not fit", "size");
        return;
    }
    if (!Update.begin(len)) {
        http.end();
        fail(500, "OTA_FAILED", Update.getErrorString(), "begin_update");
        return;
    }
    br_sha256_context sha_ctx;
    br_sha256_init(&sha_ctx);
    WiFiClient& stream = http.getStream();
    uint8_t buf[1024];
    int remaining = len;
    while (remaining > 0 && http.connected()) {
        const size_t got = stream.readBytes(buf, min(static_cast<int>(sizeof buf), remaining));
        if (!got) break;
        br_sha256_update(&sha_ctx, buf, got);
        if (Update.write(buf, got) != got) break;
        remaining -= static_cast<int>(got);
        yield(); // feed the watchdog during the long download
    }
    http.end();
    uint8_t digest[32];
    br_sha256_out(&sha_ctx, digest);
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (remaining != 0 || sha != hex) { // checksum gate (SPEC 130)
        Update.end();
        fail(422, "VERIFY_FAILED",
             remaining ? "truncated download" : "sha256 mismatch",
             remaining ? "truncated" : "sha256_mismatch");
        return;
    }
    if (!Update.end(true)) {
        fail(500, "OTA_FAILED", Update.getErrorString(), "finalize");
        return;
    }
    d_.log->log(LogEvent::OtaSuccess, "source=url");
    server_.send(200, "application/json", "{\"ok\":true}");
    rebootAt_ = millis() + 500;
}

// ---- helpers ----------------------------------------------------------------

void WebUi::sendError(int code, const char* errCode, const char* msg) { // SPEC 156
    JsonDocument d;
    d["ok"] = false;
    d["error"]["code"] = errCode;
    d["error"]["message"] = msg;
    sendJson(d, code);
}

void WebUi::sendJson(const JsonDocument& d, int code) {
    // Heap-fragmentation guard: the Web UI polls /status every 5 s. Building a
    // String here reallocs ~10x per response (grows in ~31 B steps), each leaving
    // a hole; after a minute the largest free block collapses (measured 30% frag
    // at 19 KB free) and the next TLS poll can't get its contiguous 4 KB RX buffer.
    // Small responses (status/system/mute/errors, < ~430 B) serialize into a stack
    // buffer and stream out with chunked encoding: zero heap, zero fragmentation.
    const size_t len = measureJson(d);
    if (len < 768) {
        char buf[768];
        serializeJson(d, buf, sizeof buf);
        server_.setContentLength(len);
        server_.send(code, "application/json", "");
        server_.sendContent(buf, len);
        server_.sendContent("");
        return;
    }
    // Rare large docs (config ~2.3 KB): one exact allocation, no realloc churn.
    String out;
    out.reserve(len + 1);
    serializeJson(d, out);
    server_.send(code, "application/json", out);
}
