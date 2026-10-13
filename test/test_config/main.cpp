// Config round-trip / validation / migration + log format
#include <unity.h>
#include <string>
#include "airalert/Config.h"
#include "airalert/EventLog.h"

using namespace airalert;

void test_defaults_valid() {
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::None, validateConfig(c));
    TEST_ASSERT_EQUAL(0, c.selectedCount);
    TEST_ASSERT_EQUAL(20, c.pollIntervalSec);
    TEST_ASSERT_EQUAL(90, c.profiles[static_cast<int>(AlertType::Nuclear)].priority);
}

void test_roundtrip_preserves_everything() {
    AppConfig c;
    strncpy(c.deviceName, "Siren-Irpin", sizeof c.deviceName);
    c.pollIntervalSec = 20;
    c.partialSiren = false;
    c.relaySingle = true;
    c.relayActiveHigh = false;
    c.relayMaxOnMs = 45000;
    c.selectedCount = 1;
    c.selected[0] = {123, LocationType::Hromada, 14, 67};
    c.alertIndicatorSteady = true;
    c.alertIndicatorActiveHigh = false;
    c.profiles[0].start = Pattern::pulses(5000, 500, 2);
    c.profiles[0].reminderIntervalMs = 600000;

    JsonDocument d;
    configToJson(c, d);
    AppConfig r;
    TEST_ASSERT_EQUAL(ConfigError::None, configFromJson(d.as<JsonVariantConst>(), r));

    TEST_ASSERT_EQUAL_STRING("Siren-Irpin", r.deviceName);
    TEST_ASSERT_EQUAL(20, r.pollIntervalSec);
    TEST_ASSERT_FALSE(r.partialSiren);
    TEST_ASSERT_TRUE(r.relaySingle);
    TEST_ASSERT_FALSE(r.relayActiveHigh);
    TEST_ASSERT_EQUAL_UINT32(45000, r.relayMaxOnMs);
    TEST_ASSERT_TRUE(r.alertIndicatorSteady);
    TEST_ASSERT_FALSE(r.alertIndicatorActiveHigh);
    TEST_ASSERT_EQUAL(1, r.selectedCount);
    TEST_ASSERT_EQUAL(123, r.selected[0].uid);
    TEST_ASSERT_EQUAL(LocationType::Hromada, r.selected[0].type);
    TEST_ASSERT_EQUAL(67, r.selected[0].raionUid);
    TEST_ASSERT_EQUAL_UINT32(5000, r.profiles[0].start.firstMs(true));
    TEST_ASSERT_EQUAL(2, r.profiles[0].start.repeat);
    TEST_ASSERT_EQUAL_UINT32(600000, r.profiles[0].reminderIntervalMs);
    TEST_ASSERT_EQUAL(ConfigError::None, validateConfig(r));
}

void test_partial_old_config_keeps_defaults() { // SPEC 90: N+1 reads N
    const char* old = R"({"schema":1,"alerts":{"poll_sec":30}})";
    JsonDocument d;
    TEST_ASSERT_TRUE(deserializeJson(d, old) == DeserializationError::Ok);
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::None, configFromJson(d.as<JsonVariantConst>(), c));
    TEST_ASSERT_EQUAL(30, c.pollIntervalSec);         // taken from file (not a default: kept)
    TEST_ASSERT_EQUAL(90, c.apiStaleAfterSec);        // default kept
    TEST_ASSERT_EQUAL(2, c.schemaVersion);            // migrated
    TEST_ASSERT_EQUAL(0, c.selectedCount);            // no implicit Kyiv subscription
    TEST_ASSERT_FALSE(c.relaySingle);                 // pulse-wired sirens stay two-relay
    TEST_ASSERT_TRUE(c.profiles[0].enabled);
}

void test_json_rejects_too_many_locations_without_truncating() {
    JsonDocument d;
    JsonArray locs = d["locations"].to<JsonArray>();
    for (uint8_t i = 0; i <= SnapshotBuilder::kMaxSelected; ++i) {
        JsonObject o = locs.add<JsonObject>();
        o["uid"] = 100 + i;
        o["type"] = "oblast";
    }
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::TooManyLocations,
                      configFromJson(d.as<JsonVariantConst>(), c));
    TEST_ASSERT_EQUAL(0, c.selectedCount);
}

void test_json_rejects_invalid_and_duplicate_locations() {
    JsonDocument d;
    JsonArray locs = d["locations"].to<JsonArray>();
    JsonObject a = locs.add<JsonObject>();
    a["uid"] = 14;
    a["type"] = "oblast";
    JsonObject b = locs.add<JsonObject>();
    b["uid"] = 14;
    b["type"] = "oblast";
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::BadLocation,
                      configFromJson(d.as<JsonVariantConst>(), c));

    d.clear();
    JsonObject bad = d["locations"].to<JsonArray>().add<JsonObject>();
    bad["uid"] = 123;
    bad["type"] = "not-a-location-type";
    TEST_ASSERT_EQUAL(ConfigError::BadLocation,
                      configFromJson(d.as<JsonVariantConst>(), c));
}

void test_json_rejects_overflow_and_overlong_name() {
    JsonDocument d;
    d["alerts"]["poll_sec"] = 65551;
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::BadPollInterval,
                      configFromJson(d.as<JsonVariantConst>(), c));

    d.clear();
    d["device"]["name"] = "this-device-name-is-far-too-long-for-the-buffer";
    TEST_ASSERT_EQUAL(ConfigError::BadDeviceName,
                      configFromJson(d.as<JsonVariantConst>(), c));
}

void test_json_rejects_future_schema_and_malformed_sections() {
    AppConfig c;
    JsonDocument future;
    future["schema"] = kConfigSchema + 1;
    TEST_ASSERT_EQUAL(ConfigError::BadSchema,
                      configFromJson(future.as<JsonVariantConst>(), c));

    JsonDocument malformed;
    malformed["alerts"] = "not-an-object";
    TEST_ASSERT_NOT_EQUAL(ConfigError::None,
                          configFromJson(malformed.as<JsonVariantConst>(), c));
}

void test_validation_rejects_stale_range_and_location_shape() {
    AppConfig c;
    c.apiStaleAfterSec = 10;
    TEST_ASSERT_EQUAL(ConfigError::BadStaleInterval, validateConfig(c));
    c = AppConfig{};
    c.selectedCount = 1;
    c.selected[0] = {14, LocationType::Unknown, 0, 0};
    TEST_ASSERT_EQUAL(ConfigError::BadLocation, validateConfig(c));
}

void test_schema1_default_cadence_migrates_to_schema2() { // ukrainealarm quota
    const char* old = R"({"schema":1,"alerts":{"poll_sec":15,"stale_sec":60}})";
    JsonDocument d;
    TEST_ASSERT_TRUE(deserializeJson(d, old) == DeserializationError::Ok);
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::None, configFromJson(d.as<JsonVariantConst>(), c));
    TEST_ASSERT_EQUAL(20, c.pollIntervalSec);
    TEST_ASSERT_EQUAL(90, c.apiStaleAfterSec);
    const char* cur = R"({"schema":2,"alerts":{"poll_sec":15,"stale_sec":60}})";
    TEST_ASSERT_TRUE(deserializeJson(d, cur) == DeserializationError::Ok);
    AppConfig c2;
    TEST_ASSERT_EQUAL(ConfigError::None, configFromJson(d.as<JsonVariantConst>(), c2));
    TEST_ASSERT_EQUAL(15, c2.pollIntervalSec); // explicit choice on schema 2 stays
}

void test_validation_rejects_bad_ranges() { // SPEC 7, 42, 155
    AppConfig c;
    c.pollIntervalSec = 5; // below API-safe minimum
    TEST_ASSERT_EQUAL(ConfigError::BadPollInterval, validateConfig(c));
    c = AppConfig{};
    c.relayMaxOnMs = 300000; // above hard cap 60 s
    TEST_ASSERT_EQUAL(ConfigError::BadRelayLimit, validateConfig(c));
    c = AppConfig{};
    c.profiles[2].start.steps[0] = 7000; // 70 s ON
    TEST_ASSERT_EQUAL(ConfigError::BadPattern, validateConfig(c));
    c = AppConfig{};
    c.startConfirmations = 0;
    TEST_ASSERT_EQUAL(ConfigError::BadConfirmations, validateConfig(c));
}

void test_pattern_steps_roundtrip() {
    AppConfig c;
    Pattern& s = c.profiles[0].end;
    s = Pattern{};
    s.add(true, 13000);
    s.add(false, 3000);
    s.add(true, 10000);
    s.repeat = 1;
    JsonDocument d;
    configToJson(c, d);
    JsonArrayConst steps = d["profiles"]["air_raid"]["end"]["steps"];
    TEST_ASSERT_EQUAL(3, steps.size());
    TEST_ASSERT_EQUAL_INT32(13000, steps[0].as<int32_t>());
    TEST_ASSERT_EQUAL_INT32(-3000, steps[1].as<int32_t>());
    AppConfig r;
    TEST_ASSERT_EQUAL(ConfigError::None, configFromJson(d.as<JsonVariantConst>(), r));
    TEST_ASSERT_EQUAL(3, r.profiles[0].end.count);
    TEST_ASSERT_EQUAL_UINT32(10000, r.profiles[0].end.stepMs(2));
    TEST_ASSERT_FALSE(r.profiles[0].end.stepOn(1));
}

void test_legacy_on_off_pattern_migrates_to_steps() { // 0.9.2 configs on flash
    const char* old = R"({"profiles":{"air_raid":{"start":{"enabled":true,"on_ms":5000,"off_ms":2000,"repeat":3},"end":{"on_ms":0}}}})";
    JsonDocument d;
    TEST_ASSERT_TRUE(deserializeJson(d, old) == DeserializationError::Ok);
    AppConfig c;
    TEST_ASSERT_EQUAL(ConfigError::None, configFromJson(d.as<JsonVariantConst>(), c));
    const Pattern& s = c.profiles[0].start;
    TEST_ASSERT_EQUAL(2, s.count);
    TEST_ASSERT_EQUAL_UINT32(5000, s.stepMs(0));
    TEST_ASSERT_TRUE(s.stepOn(0));
    TEST_ASSERT_EQUAL_UINT32(2000, s.stepMs(1));
    TEST_ASSERT_EQUAL(3, s.repeat);
    // on_ms alone keeps the default pause; on_ms 0 leaves just the pause
    TEST_ASSERT_FALSE(c.profiles[0].end.hasOn());
}

void test_pattern_steps_reject_bad_values() {
    const char* bad[] = {
        R"({"profiles":{"air_raid":{"start":{"steps":[1000,0]}}}})",
        R"({"profiles":{"air_raid":{"start":{"steps":[60010]}}}})",
        R"({"profiles":{"air_raid":{"start":{"steps":[-60010]}}}})",
        R"({"profiles":{"air_raid":{"start":{"steps":[1000.5]}}}})",
        R"({"profiles":{"air_raid":{"start":{"steps":"1000"}}}})",
        R"({"profiles":{"air_raid":{"start":{"steps":[1,1,1,1,1,1,1,1,1,1,1,1,1]}}}})",
        R"({"profiles":{"air_raid":{"start":{"steps":[1000],"repeat":21}}}})",
    };
    for (const char* json : bad) {
        JsonDocument d;
        TEST_ASSERT_TRUE(deserializeJson(d, json) == DeserializationError::Ok);
        AppConfig c;
        TEST_ASSERT_EQUAL_MESSAGE(ConfigError::BadPattern,
                                  configFromJson(d.as<JsonVariantConst>(), c), json);
    }
}

void test_log_record_format() { // SPEC 94
    char buf[160];
    size_t n = formatLogRecord(buf, sizeof buf, 1755763200, LogEvent::AlertStart,
                               "type=air_raid");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1755763200,\"event\":\"ALERT_START\",\"detail\":\"type=air_raid\"}\n", buf);
    n = formatLogRecord(buf, sizeof buf, 0, LogEvent::Boot, nullptr);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":0,\"event\":\"BOOT\"}\n", buf);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_valid);
    RUN_TEST(test_roundtrip_preserves_everything);
    RUN_TEST(test_partial_old_config_keeps_defaults);
    RUN_TEST(test_schema1_default_cadence_migrates_to_schema2);
    RUN_TEST(test_validation_rejects_bad_ranges);
    RUN_TEST(test_json_rejects_too_many_locations_without_truncating);
    RUN_TEST(test_json_rejects_invalid_and_duplicate_locations);
    RUN_TEST(test_json_rejects_overflow_and_overlong_name);
    RUN_TEST(test_json_rejects_future_schema_and_malformed_sections);
    RUN_TEST(test_validation_rejects_stale_range_and_location_shape);
    RUN_TEST(test_pattern_steps_roundtrip);
    RUN_TEST(test_legacy_on_off_pattern_migrates_to_steps);
    RUN_TEST(test_pattern_steps_reject_bad_values);
    RUN_TEST(test_log_record_format);
    return UNITY_END();
}
