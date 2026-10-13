#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <ArduinoJson.h>
#include "AlertProfiles.h"
#include "Location.h"
#include "SnapshotBuilder.h"
#include "Types.h"

namespace airalert {

constexpr uint8_t kConfigSchema = 2;

// Whole-app configuration (SPEC 121 defaults, 175 separation).
// Secrets live in /secrets.json, never here (SPEC 91, 176).
struct AppConfig {
    uint8_t schemaVersion = kConfigSchema;
    char deviceName[32] = "AirAlert";

    // alerts. ukrainealarm.com accepts ~3 requests/min per key and answers
    // the rest with 401 (docs/RESEARCH.md): 20 s = 3/min, ~2 of them accepted.
    uint16_t pollIntervalSec = 20;   // min 10 (SPEC 7)
    uint16_t apiStaleAfterSec = 90;  // survives 3 rate-limited polls in a row
    uint8_t startConfirmations = 1;
    uint8_t endConfirmations = 2;
    bool partialActive = true;
    bool partialSiren = true;
    bool partialLed = true;
    bool notifyEscalation = true;
    bool notifyAdditionalLocation = false;
    bool remindersWhenStale = false;

    // relay (SPEC 39, 41-42, 54). single: one relay on D5 holds the siren ON
    // by level; otherwise two relays pulse the START (D5) / STOP (D6) buttons.
    // Default stays two-relay: a pulse-wired siren read as level mode would
    // get START held and never see STOP.
    bool relaySingle = false;
    bool relayActiveHigh = true;
    uint32_t relayMaxOnMs = 30000;
    uint16_t relayTestMs = 600;

    // mute (SPEC 52-53)
    uint16_t snoozeMinutes = 15;
    bool muteAllAlertTypes = false;

    // indication (simplified two-channel scheme; rgb mode reserved for the
    // future WS2812/MCP23017 backend)
    bool alertIndicatorSteady = false;     // true when it drives a relay/lamp
    bool alertIndicatorActiveHigh = true;

    // selected locations (SPEC 21)
    Location selected[SnapshotBuilder::kMaxSelected];
    uint8_t selectedCount = 0;

    AlertProfile profiles[kAlertTypeCount];

    AppConfig() {
        for (uint8_t i = 0; i < kAlertTypeCount; ++i)
            profiles[i] = defaultProfile(static_cast<AlertType>(i));
    }
};

enum class ConfigError : uint8_t {
    None, BadSchema, BadDeviceName, BadPollInterval, BadStaleInterval, BadConfirmations,
    BadRelayLimit, BadTestMs, BadSnooze, TooManyLocations, BadLocation,
    BadPattern
};

// Hard limits (SPEC 42, 155): backend validation, never UI-only.
inline ConfigError validateConfig(const AppConfig& c) {
    if (c.schemaVersion == 0 || c.schemaVersion > kConfigSchema)
        return ConfigError::BadSchema;
    const size_t nameLen = strnlen(c.deviceName, sizeof c.deviceName);
    if (nameLen == 0 || nameLen >= sizeof c.deviceName) return ConfigError::BadDeviceName;
    if (c.pollIntervalSec < 10 || c.pollIntervalSec > 600) return ConfigError::BadPollInterval;
    if (c.apiStaleAfterSec < 30 || c.apiStaleAfterSec > 600)
        return ConfigError::BadStaleInterval;
    if (c.startConfirmations < 1 || c.startConfirmations > 5 ||
        c.endConfirmations < 1 || c.endConfirmations > 10) return ConfigError::BadConfirmations;
    if (c.relayMaxOnMs < 1000 || c.relayMaxOnMs > 60000) return ConfigError::BadRelayLimit;
    if (c.relayTestMs < 100 || c.relayTestMs > 10000) return ConfigError::BadTestMs;
    if (c.snoozeMinutes < 1 || c.snoozeMinutes > 240) return ConfigError::BadSnooze;
    if (c.selectedCount > SnapshotBuilder::kMaxSelected) return ConfigError::TooManyLocations;
    for (uint8_t i = 0; i < c.selectedCount; ++i) {
        const Location& loc = c.selected[i];
        if (loc.uid == 0 || loc.type == LocationType::Unknown)
            return ConfigError::BadLocation;
        if (loc.type == LocationType::Raion && loc.oblastUid == 0)
            return ConfigError::BadLocation;
        if (loc.type == LocationType::Hromada &&
            (loc.oblastUid == 0 || loc.raionUid == 0))
            return ConfigError::BadLocation;
        for (uint8_t j = 0; j < i; ++j)
            if (c.selected[j].uid == loc.uid) return ConfigError::BadLocation;
    }
    for (const auto& p : c.profiles) {
        for (const Pattern* pt : {&p.start, &p.end, &p.reminder}) {
            if (pt->count > Pattern::kMaxSteps || pt->repeat > 20)
                return ConfigError::BadPattern;
            for (uint8_t i = 0; i < pt->count; ++i)
                if (pt->steps[i] == 0 || pt->stepMs(i) > Pattern::kMaxStepMs)
                    return ConfigError::BadPattern;
        }
        if (p.reminderIntervalMs < 30000) return ConfigError::BadPattern;
    }
    return ConfigError::None;
}

// ---- JSON (de)serialization -------------------------------------------------

// steps: signed ms, > 0 siren ON, < 0 pause, e.g. [13000, -3000, 10000].
inline void patternToJson(JsonObject o, const Pattern& p) {
    o["enabled"] = p.enabled;
    o["repeat"] = p.repeat;
    JsonArray steps = o["steps"].to<JsonArray>();
    for (uint8_t i = 0; i < p.count; ++i)
        steps.add(p.stepOn(i) ? static_cast<int32_t>(p.stepMs(i))
                              : -static_cast<int32_t>(p.stepMs(i)));
}
template <typename T>
inline bool overlayUnsigned(JsonVariantConst v, T& out) {
    if (v.isNull()) return true;
    if (!v.is<uint32_t>()) return false;
    const uint32_t value = v.as<uint32_t>();
    if (value > static_cast<uint32_t>(std::numeric_limits<T>::max())) return false;
    out = static_cast<T>(value);
    return true;
}

inline bool overlayBool(JsonVariantConst v, bool& out) {
    if (v.isNull()) return true;
    if (!v.is<bool>()) return false;
    out = v.as<bool>();
    return true;
}

// Accepts `steps`, or the on_ms/off_ms pair of firmware <= 0.9.2 (configs on flash and
// old API clients), which becomes the equivalent two-step sequence.
inline bool patternFromJson(JsonVariantConst o, Pattern& p) {
    if (o.isNull()) return true;
    if (!o.is<JsonObjectConst>()) return false;
    if (!overlayBool(o["enabled"], p.enabled) || !overlayUnsigned(o["repeat"], p.repeat))
        return false;
    JsonVariantConst stepsValue = o["steps"];
    if (!stepsValue.isNull()) {
        if (!stepsValue.is<JsonArrayConst>()) return false;
        JsonArrayConst arr = stepsValue.as<JsonArrayConst>();
        if (arr.size() > Pattern::kMaxSteps) return false;
        Pattern next = p;
        next.count = 0;
        for (JsonVariantConst v : arr) {
            if (!v.is<int32_t>()) return false;
            const int32_t ms = v.as<int32_t>();
            if (ms == 0 || ms > static_cast<int32_t>(Pattern::kMaxStepMs) ||
                ms < -static_cast<int32_t>(Pattern::kMaxStepMs))
                return false;
            if (!next.add(ms > 0, static_cast<uint32_t>(ms > 0 ? ms : -ms))) return false;
        }
        p = next;
        return true;
    }
    if (o["on_ms"].isNull() && o["off_ms"].isNull()) return true;
    uint32_t onMs = p.firstMs(true), offMs = p.firstMs(false);
    if (!overlayUnsigned(o["on_ms"], onMs) || !overlayUnsigned(o["off_ms"], offMs) ||
        onMs > Pattern::kMaxStepMs || offMs > Pattern::kMaxStepMs)
        return false;
    p = Pattern::pulses(onMs, offMs, p.repeat, p.enabled);
    return true;
}

inline const char* locationTypeToString(LocationType t) {
    switch (t) {
        case LocationType::Oblast: return "oblast";
        case LocationType::Raion: return "raion";
        case LocationType::Hromada: return "hromada";
        case LocationType::City: return "city";
        default: return "unknown";
    }
}

inline void configToJson(const AppConfig& c, JsonDocument& d) {
    d["schema"] = c.schemaVersion;
    d["device"]["name"] = c.deviceName;
    JsonObject a = d["alerts"].to<JsonObject>();
    a["poll_sec"] = c.pollIntervalSec;
    a["stale_sec"] = c.apiStaleAfterSec;
    a["start_conf"] = c.startConfirmations;
    a["end_conf"] = c.endConfirmations;
    a["partial_active"] = c.partialActive;
    a["partial_siren"] = c.partialSiren;
    a["partial_led"] = c.partialLed;
    a["notify_escalation"] = c.notifyEscalation;
    a["notify_additional_location"] = c.notifyAdditionalLocation;
    a["reminders_when_stale"] = c.remindersWhenStale;
    JsonObject r = d["relay"].to<JsonObject>();
    r["single"] = c.relaySingle;
    r["active_high"] = c.relayActiveHigh;
    r["max_on_ms"] = c.relayMaxOnMs;
    r["test_ms"] = c.relayTestMs;
    d["mute"]["snooze_min"] = c.snoozeMinutes;
    d["mute"]["all_types"] = c.muteAllAlertTypes;
    d["led"]["alert_steady"] = c.alertIndicatorSteady;
    d["led"]["alert_active_high"] = c.alertIndicatorActiveHigh;
    JsonArray locs = d["locations"].to<JsonArray>();
    for (uint8_t i = 0; i < c.selectedCount; ++i) {
        JsonObject o = locs.add<JsonObject>();
        o["uid"] = c.selected[i].uid;
        o["type"] = locationTypeToString(c.selected[i].type);
        if (c.selected[i].oblastUid) o["oblast_uid"] = c.selected[i].oblastUid;
        if (c.selected[i].raionUid) o["raion_uid"] = c.selected[i].raionUid;
    }
    JsonObject profs = d["profiles"].to<JsonObject>();
    for (uint8_t i = 0; i < kAlertTypeCount; ++i) {
        const AlertProfile& p = c.profiles[i];
        JsonObject o = profs[alertTypeToString(static_cast<AlertType>(i))].to<JsonObject>();
        o["enabled"] = p.enabled;
        o["priority"] = p.priority;
        patternToJson(o["start"].to<JsonObject>(), p.start);
        patternToJson(o["end"].to<JsonObject>(), p.end);
        patternToJson(o["reminder"].to<JsonObject>(), p.reminder);
        o["reminder"]["interval_sec"] = p.reminderIntervalMs / 1000;
    }
}

// Missing fields keep defaults, so older configs load cleanly (SPEC 90).
// Invalid values fail instead of being narrowed or silently truncated.
inline ConfigError configFromJson(JsonVariantConst d, AppConfig& c) {
    if (!d.is<JsonObjectConst>()) return ConfigError::BadPattern;
    if (!overlayUnsigned(d["schema"], c.schemaVersion)) return ConfigError::BadSchema;
    for (const char* section : {"device", "alerts", "relay", "mute", "led"})
        if (!d[section].isNull() && !d[section].is<JsonObjectConst>())
            return ConfigError::BadPattern;
    const char* name = d["device"]["name"] | static_cast<const char*>(nullptr);
    if (name) {
        const size_t len = strlen(name);
        if (len == 0 || len >= sizeof c.deviceName) return ConfigError::BadDeviceName;
        memcpy(c.deviceName, name, len + 1);
    } else if (!d["device"]["name"].isNull()) {
        return ConfigError::BadDeviceName;
    }
    JsonVariantConst a = d["alerts"];
    if (!overlayUnsigned(a["poll_sec"], c.pollIntervalSec)) return ConfigError::BadPollInterval;
    if (!overlayUnsigned(a["stale_sec"], c.apiStaleAfterSec)) return ConfigError::BadStaleInterval;
    if (!overlayUnsigned(a["start_conf"], c.startConfirmations) ||
        !overlayUnsigned(a["end_conf"], c.endConfirmations))
        return ConfigError::BadConfirmations;
    if (!overlayBool(a["partial_active"], c.partialActive) ||
        !overlayBool(a["partial_siren"], c.partialSiren) ||
        !overlayBool(a["partial_led"], c.partialLed) ||
        !overlayBool(a["notify_escalation"], c.notifyEscalation) ||
        !overlayBool(a["notify_additional_location"], c.notifyAdditionalLocation) ||
        !overlayBool(a["reminders_when_stale"], c.remindersWhenStale))
        return ConfigError::BadPattern;
    JsonVariantConst r = d["relay"];
    if (!overlayBool(r["single"], c.relaySingle) ||
        !overlayBool(r["active_high"], c.relayActiveHigh)) return ConfigError::BadRelayLimit;
    if (!overlayUnsigned(r["max_on_ms"], c.relayMaxOnMs)) return ConfigError::BadRelayLimit;
    if (!overlayUnsigned(r["test_ms"], c.relayTestMs)) return ConfigError::BadTestMs;
    if (!overlayUnsigned(d["mute"]["snooze_min"], c.snoozeMinutes) ||
        !overlayBool(d["mute"]["all_types"], c.muteAllAlertTypes))
        return ConfigError::BadSnooze;
    if (!overlayBool(d["led"]["alert_steady"], c.alertIndicatorSteady) ||
        !overlayBool(d["led"]["alert_active_high"], c.alertIndicatorActiveHigh))
        return ConfigError::BadPattern;

    JsonVariantConst locValue = d["locations"];
    if (!locValue.isNull()) {
        if (!locValue.is<JsonArrayConst>()) return ConfigError::BadLocation;
        JsonArrayConst locs = locValue.as<JsonArrayConst>();
        if (locs.size() > SnapshotBuilder::kMaxSelected)
            return ConfigError::TooManyLocations;
        Location parsed[SnapshotBuilder::kMaxSelected];
        uint8_t parsedCount = 0;
        for (JsonVariantConst item : locs) {
            if (!item.is<JsonObjectConst>()) return ConfigError::BadLocation;
            JsonObjectConst o = item.as<JsonObjectConst>();
            Location loc;
            if (!overlayUnsigned(o["uid"], loc.uid) ||
                !overlayUnsigned(o["oblast_uid"], loc.oblastUid) ||
                !overlayUnsigned(o["raion_uid"], loc.raionUid))
                return ConfigError::BadLocation;
            const char* type = o["type"] | static_cast<const char*>(nullptr);
            loc.type = locationTypeFromString(type);
            if (loc.uid == 0 || loc.type == LocationType::Unknown)
                return ConfigError::BadLocation;
            for (uint8_t i = 0; i < parsedCount; ++i)
                if (parsed[i].uid == loc.uid) return ConfigError::BadLocation;
            parsed[parsedCount++] = loc;
        }
        memcpy(c.selected, parsed, sizeof(Location) * parsedCount);
        c.selectedCount = parsedCount;
    }
    JsonVariantConst profs = d["profiles"];
    if (!profs.isNull()) {
        if (!profs.is<JsonObjectConst>()) return ConfigError::BadPattern;
        for (uint8_t i = 0; i < kAlertTypeCount; ++i) {
            JsonVariantConst o = profs[alertTypeToString(static_cast<AlertType>(i))];
            if (o.isNull()) continue;
            if (!o.is<JsonObjectConst>()) return ConfigError::BadPattern;
            AlertProfile& p = c.profiles[i];
            if (!overlayBool(o["enabled"], p.enabled) ||
                !overlayUnsigned(o["priority"], p.priority) ||
                !patternFromJson(o["start"], p.start) ||
                !patternFromJson(o["end"], p.end) ||
                !patternFromJson(o["reminder"], p.reminder))
                return ConfigError::BadPattern;
            uint32_t intervalSec = p.reminderIntervalMs / 1000;
            if (!overlayUnsigned(o["reminder"]["interval_sec"], intervalSec) ||
                intervalSec > UINT32_MAX / 1000UL)
                return ConfigError::BadPattern;
            p.reminderIntervalMs = intervalSec * 1000UL;
        }
    }
    // schema 1 -> 2: the alerts.in.ua-era 15 s / 60 s defaults poll ukrainealarm
    // past its ~3/min quota; lift only untouched defaults (SPEC 90 migration).
    if (c.schemaVersion == 1) {
        if (c.pollIntervalSec == 15) c.pollIntervalSec = 20;
        if (c.apiStaleAfterSec == 60) c.apiStaleAfterSec = 90;
        c.schemaVersion = 2;
    }
    return validateConfig(c);
}

} // namespace airalert
