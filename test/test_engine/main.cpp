// AlertEngine tests — SPEC 136
#include <unity.h>
#include "airalert/AlertEngine.h"

using namespace airalert;

static constexpr uint8_t AIR = static_cast<uint8_t>(AlertType::AirRaid);
static constexpr uint8_t CHEM = static_cast<uint8_t>(AlertType::Chemical);

static AlertEngine::Snapshot snap(Coverage air, uint8_t locs = 1) {
    AlertEngine::Snapshot s{};
    s.types[AIR] = {air, air == Coverage::None ? uint8_t(0) : locs, 1000};
    return s;
}

static size_t apply(AlertEngine& e, const AlertEngine::Snapshot& s, EngineEvent* ev) {
    return e.applySnapshot(s, ev, 8);
}

void test_inactive_to_start() {
    AlertEngine e;
    EngineEvent ev[8];
    size_t n = apply(e, snap(Coverage::Full), ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::Started, ev[0].kind);
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::AirRaid).state);
    TEST_ASSERT_EQUAL_INT64(1000, e.status(AlertType::AirRaid).startedAt);
}

void test_start_needs_confirmations() {
    AlertEngine e({2, 2, true});
    EngineEvent ev[8];
    TEST_ASSERT_EQUAL(0, apply(e, snap(Coverage::Full), ev));
    TEST_ASSERT_EQUAL(AlertState::Inactive, e.status(AlertType::AirRaid).state);
    size_t n = apply(e, snap(Coverage::Full), ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::Started, ev[0].kind);
}

void test_active_to_end_two_confirmations() {
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Full), ev);
    TEST_ASSERT_EQUAL(0, apply(e, snap(Coverage::None), ev)); // 1st miss: still active
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::AirRaid).state);
    size_t n = apply(e, snap(Coverage::None), ev);            // 2nd miss: end
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::Ended, ev[0].kind);
    TEST_ASSERT_EQUAL(AlertState::Inactive, e.status(AlertType::AirRaid).state);
}

void test_blip_does_not_end() { // active -> one miss -> active again
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Full), ev);
    apply(e, snap(Coverage::None), ev);
    TEST_ASSERT_EQUAL(0, apply(e, snap(Coverage::Full), ev)); // recovered, no events
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::AirRaid).state);
    apply(e, snap(Coverage::None), ev);                        // counter must be reset
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::AirRaid).state);
}

void test_partial_to_full_escalates() { // SPEC 35
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Partial), ev);
    size_t n = apply(e, snap(Coverage::Full), ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::Escalated, ev[0].kind);
}

void test_full_to_partial_no_end() { // SPEC 36
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Full), ev);
    size_t n = apply(e, snap(Coverage::Partial), ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::CoverageReduced, ev[0].kind);
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::AirRaid).state);
}

void test_partial_disabled() { // SPEC 20
    AlertEngine e({1, 2, false});
    EngineEvent ev[8];
    TEST_ASSERT_EQUAL(0, apply(e, snap(Coverage::Partial), ev));
    TEST_ASSERT_EQUAL(AlertState::Inactive, e.status(AlertType::AirRaid).state);
}

void test_two_locations_one_clears() { // SPEC 21, Invariant 5
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Full, 2), ev);
    // one location cleared, other still active => still Full coverage, 1 loc
    size_t n = apply(e, snap(Coverage::Full, 1), ev);
    TEST_ASSERT_EQUAL(0, n);
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::AirRaid).state);
    // both cleared
    apply(e, snap(Coverage::None), ev);
    n = apply(e, snap(Coverage::None), ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::Ended, ev[0].kind);
}

void test_additional_location() { // SPEC 37
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Full, 1), ev);
    size_t n = apply(e, snap(Coverage::Full, 2), ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::LocationAdded, ev[0].kind);
}

void test_two_types_independent() {
    AlertEngine e;
    EngineEvent ev[8];
    AlertEngine::Snapshot s{};
    s.types[AIR] = {Coverage::Full, 1, 1000};
    s.types[CHEM] = {Coverage::Full, 1, 2000};
    size_t n = apply(e, s, ev);
    TEST_ASSERT_EQUAL(2, n);
    // air raid ends, chemical continues
    AlertEngine::Snapshot s2{};
    s2.types[CHEM] = {Coverage::Full, 1, 2000};
    apply(e, s2, ev);
    n = apply(e, s2, ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertEvent::Ended, ev[0].kind);
    TEST_ASSERT_EQUAL(AlertType::AirRaid, ev[0].type);
    TEST_ASSERT_EQUAL(AlertState::Active, e.status(AlertType::Chemical).state);
}

void test_unknown_type_slot_works() { // SPEC 136 "unknown alert type", Invariant 9
    AlertEngine e;
    EngineEvent ev[8];
    AlertEngine::Snapshot s{};
    s.types[static_cast<uint8_t>(AlertType::Unknown)] = {Coverage::Full, 1, 1000};
    size_t n = apply(e, s, ev);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(AlertType::Unknown, ev[0].type);
}

void test_unknown_until_first_sync() { // SPEC 166
    AlertEngine e;
    TEST_ASSERT_FALSE(e.synced());
    TEST_ASSERT_EQUAL(AlertState::Unknown, e.status(AlertType::AirRaid).state);
    EngineEvent ev[8];
    apply(e, snap(Coverage::None), ev);
    TEST_ASSERT_TRUE(e.synced());
    TEST_ASSERT_EQUAL(AlertState::Inactive, e.status(AlertType::AirRaid).state);
}

void test_boot_into_active_alert() { // SPEC 26: first snapshot already active
    AlertEngine e;
    EngineEvent ev[8];
    size_t n = apply(e, snap(Coverage::Full), ev);
    TEST_ASSERT_EQUAL(1, n); // engine reports Started; PreexistingAlerts silences it
    TEST_ASSERT_TRUE(e.anyActive());
}

void test_reset_returns_engine_to_unknown() {
    AlertEngine e;
    EngineEvent ev[8];
    apply(e, snap(Coverage::Full), ev);
    TEST_ASSERT_TRUE(e.anyActive());
    e.reset();
    TEST_ASSERT_FALSE(e.synced());
    TEST_ASSERT_FALSE(e.anyActive());
    TEST_ASSERT_EQUAL(AlertState::Unknown, e.status(AlertType::AirRaid).state);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_inactive_to_start);
    RUN_TEST(test_start_needs_confirmations);
    RUN_TEST(test_active_to_end_two_confirmations);
    RUN_TEST(test_blip_does_not_end);
    RUN_TEST(test_partial_to_full_escalates);
    RUN_TEST(test_full_to_partial_no_end);
    RUN_TEST(test_partial_disabled);
    RUN_TEST(test_two_locations_one_clears);
    RUN_TEST(test_additional_location);
    RUN_TEST(test_two_types_independent);
    RUN_TEST(test_unknown_type_slot_works);
    RUN_TEST(test_unknown_until_first_sync);
    RUN_TEST(test_boot_into_active_alert);
    RUN_TEST(test_reset_returns_engine_to_unknown);
    return UNITY_END();
}
