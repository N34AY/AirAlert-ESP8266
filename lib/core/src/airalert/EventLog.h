#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace airalert {

// Event vocabulary (SPEC 93). Polls are never logged.
enum class LogEvent : uint8_t {
    Boot, WifiConnected, WifiDisconnected,
    ApiOnline, ApiOffline, Api401, Api403, Api429, ApiParseError,
    AlertStart, AlertPartial, AlertFull, AlertEnd,
    Mute, Unmute, Test, ConfigChanged,
    OtaStarted, OtaSuccess, OtaFailed, FactoryReset, RelaySafetyTrip,
    RelayPress
};

inline const char* logEventName(LogEvent e) {
    switch (e) {
        case LogEvent::Boot: return "BOOT";
        case LogEvent::WifiConnected: return "WIFI_CONNECTED";
        case LogEvent::WifiDisconnected: return "WIFI_DISCONNECTED";
        case LogEvent::ApiOnline: return "API_ONLINE";
        case LogEvent::ApiOffline: return "API_OFFLINE";
        case LogEvent::Api401: return "API_401";
        case LogEvent::Api403: return "API_403";
        case LogEvent::Api429: return "API_429";
        case LogEvent::ApiParseError: return "API_PARSE_ERROR";
        case LogEvent::AlertStart: return "ALERT_START";
        case LogEvent::AlertPartial: return "ALERT_PARTIAL";
        case LogEvent::AlertFull: return "ALERT_FULL";
        case LogEvent::AlertEnd: return "ALERT_END";
        case LogEvent::Mute: return "MUTE";
        case LogEvent::Unmute: return "UNMUTE";
        case LogEvent::Test: return "TEST";
        case LogEvent::ConfigChanged: return "CONFIG_CHANGED";
        case LogEvent::OtaStarted: return "OTA_STARTED";
        case LogEvent::OtaSuccess: return "OTA_SUCCESS";
        case LogEvent::OtaFailed: return "OTA_FAILED";
        case LogEvent::FactoryReset: return "FACTORY_RESET";
        case LogEvent::RelaySafetyTrip: return "RELAY_SAFETY_TRIP";
        case LogEvent::RelayPress: return "RELAY_PRESS";
    }
    return "?";
}

// One JSON line per record (SPEC 94). Returns bytes written (excl. NUL).
// ts: unix seconds, 0 = clock not synced yet (record still written).
// detail: optional extra ("type=air_raid"), may be nullptr. No secrets ever.
inline size_t formatLogRecord(char* buf, size_t cap, int64_t ts, LogEvent e,
                              const char* detail) {
    const int n = snprintf(buf, cap, "{\"ts\":%lld,\"event\":\"%s\"%s%s%s}\n",
                           static_cast<long long>(ts), logEventName(e),
                           detail ? ",\"detail\":\"" : "",
                           detail ? detail : "",
                           detail ? "\"" : "");
    return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

} // namespace airalert
