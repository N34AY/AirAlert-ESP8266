// NotificationEngine + Debouncer — SPEC 26, 47, 50-54, 117
#include <unity.h>
#include "airalert/NotificationEngine.h"
#include "airalert/Debouncer.h"

using namespace airalert;

static EngineEvent started(AlertType t) { return {AlertEvent::Started, t}; }
static EngineEvent ended(AlertType t) { return {AlertEvent::Ended, t}; }
static EngineEvent escalated(AlertType t) { return {AlertEvent::Escalated, t}; }
static EngineEvent locationAdded(AlertType t) { return {AlertEvent::LocationAdded, t}; }

void test_start_plays_start_pattern() {
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    TEST_ASSERT_TRUE(n.tick(0));      // ON (3000 ms phase)
    TEST_ASSERT_TRUE(n.tick(2999));
    TEST_ASSERT_FALSE(n.tick(3500));  // OFF gap
    TEST_ASSERT_TRUE(n.tick(4200));   // second repeat
}

void test_silent_start_makes_no_sound_but_end_does() { // SPEC 26
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Silent, 0);
    TEST_ASSERT_FALSE(n.tick(0));
    TEST_ASSERT_EQUAL(0, n.queued());
    n.onEngineEvent(ended(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 5000);
    TEST_ASSERT_TRUE(n.tick(5000)); // the all-clear came after boot: voiced
}

void test_mute_silences_immediately() { // Invariant 4
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    TEST_ASSERT_TRUE(n.tick(0));
    n.muteShort(100);
    TEST_ASSERT_FALSE(n.tick(101));
    TEST_ASSERT_TRUE(n.muted());
    TEST_ASSERT_EQUAL(0, n.queued());
}

void test_new_type_overrides_mute() { // SPEC 53
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    n.tick(0);
    n.muteShort(100);
    n.onEngineEvent(started(AlertType::Chemical), NotificationEngine::StartupMode::Normal, 200);
    TEST_ASSERT_TRUE(n.tick(200)); // chemical sounds despite mute
}

void test_muted_type_stays_silent_until_clear() { // SPEC 52
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    n.tick(0);
    n.muteShort(100);
    // reminder would fire but is silenced; end clears the mute
    n.onEngineEvent(ended(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 5000);
    TEST_ASSERT_FALSE(n.muted()); // all clear -> unmuted
    TEST_ASSERT_TRUE(n.tick(5000)); // END pattern audible
}

void test_manual_test_sounds_when_muted() { // SPEC 54
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    n.tick(0);
    n.muteShort(100);
    n.manualTest(200);
    TEST_ASSERT_TRUE(n.tick(200));
    TEST_ASSERT_FALSE(n.tick(801)); // 600 ms pulse done
}

void test_manual_test_uses_configured_duration() {
    NotificationEngine n;
    NotificationEngine::Config cfg;
    cfg.manualTestMs = 250;
    n.setConfig(cfg);
    n.manualTest(0);
    TEST_ASSERT_TRUE(n.tick(0));
    TEST_ASSERT_TRUE(n.tick(249));
    TEST_ASSERT_FALSE(n.tick(250));
}

void test_escalation_policy_can_disable_signal() {
    NotificationEngine n;
    NotificationEngine::Config cfg;
    cfg.notifyEscalation = false;
    n.setConfig(cfg);
    n.onEngineEvent(escalated(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    TEST_ASSERT_FALSE(n.tick(0));
}

void test_additional_location_policy_can_enable_signal() {
    NotificationEngine n;
    NotificationEngine::Config cfg;
    cfg.notifyAdditionalLocation = true;
    n.setConfig(cfg);
    n.onEngineEvent(locationAdded(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    TEST_ASSERT_TRUE(n.tick(0));
}

void test_stale_data_suppresses_reminders_by_default() {
    NotificationEngine n;
    AlertProfile p = defaultProfile(AlertType::AirRaid);
    p.reminder = Pattern::pulses(500, 0, 1);
    p.reminderIntervalMs = 1000;
    n.setProfile(AlertType::AirRaid, p);
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Silent, 0);
    n.tick(0);
    n.setApiStale(true);
    TEST_ASSERT_FALSE(n.tick(1000));
    TEST_ASSERT_FALSE(n.tick(1001));
}

void test_stale_reminders_can_be_enabled() {
    NotificationEngine n;
    NotificationEngine::Config cfg;
    cfg.remindersWhenStale = true;
    n.setConfig(cfg);
    AlertProfile p = defaultProfile(AlertType::AirRaid);
    p.reminder = Pattern::pulses(500, 0, 1);
    p.reminderIntervalMs = 1000;
    n.setProfile(AlertType::AirRaid, p);
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Silent, 0);
    n.tick(0);
    n.setApiStale(true);
    TEST_ASSERT_FALSE(n.tick(999));
    TEST_ASSERT_TRUE(n.tick(1000));
}

void test_end_does_not_preempt_start() { // SPEC 47
    NotificationEngine n;
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    TEST_ASSERT_TRUE(n.tick(0)); // start pattern playing
    n.onEngineEvent(ended(AlertType::Chemical), NotificationEngine::StartupMode::Normal, 10); // end arrives
    TEST_ASSERT_TRUE(n.tick(100)); // start keeps playing
}

void test_start_preempts_reminder() { // SPEC 47
    NotificationEngine n;
    AlertProfile p = defaultProfile(AlertType::AirRaid);
    p.reminder = Pattern::pulses(10000, 0, 1); // long reminder to preempt
    p.reminderIntervalMs = 1000;
    n.setProfile(AlertType::AirRaid, p);
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Silent, 0);
    n.tick(0);
    TEST_ASSERT_TRUE(n.tick(1200)); // reminder playing (interval passed)
    n.onEngineEvent(started(AlertType::Chemical), NotificationEngine::StartupMode::Normal, 1300);
    n.tick(1300);
    TEST_ASSERT_TRUE(n.tick(1400)); // chemical start took over
    // chemical start: ON 3000 from ~1300 -> still ON at 4000
    TEST_ASSERT_TRUE(n.tick(4000));
}

void test_reminder_interval() {
    NotificationEngine n;
    AlertProfile p = defaultProfile(AlertType::AirRaid);
    p.reminder = Pattern::pulses(500, 0, 1);
    p.reminderIntervalMs = 60000;
    n.setProfile(AlertType::AirRaid, p);
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Silent, 0);
    n.tick(0);
    TEST_ASSERT_FALSE(n.tick(30000)); // not yet
    TEST_ASSERT_TRUE(n.tick(60001)); // reminder fires
}

void test_disabled_profile_no_sound() {
    NotificationEngine n;
    AlertProfile p = defaultProfile(AlertType::AirRaid);
    p.enabled = false;
    n.setProfile(AlertType::AirRaid, p);
    n.onEngineEvent(started(AlertType::AirRaid), NotificationEngine::StartupMode::Normal, 0);
    TEST_ASSERT_FALSE(n.tick(0));
}

void test_debouncer_press_release() {
    Debouncer d;
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(true, 0));
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(true, 39));
    TEST_ASSERT_EQUAL(Debouncer::Event::Press, d.update(true, 40));
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(true, 100));
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(false, 200));
    TEST_ASSERT_EQUAL(Debouncer::Event::Release, d.update(false, 240));
    TEST_ASSERT_TRUE(d.wasShortPress());
}

void test_debouncer_bounce_ignored() {
    Debouncer d;
    d.update(true, 0);
    d.update(false, 10); // bounce
    d.update(true, 20);
    d.update(false, 30);
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(false, 100)); // never stabilized pressed
    TEST_ASSERT_FALSE(d.pressed());
}

void test_debouncer_long_press() { // SPEC 54: hold 2 s
    Debouncer d;
    d.update(true, 0);
    TEST_ASSERT_EQUAL(Debouncer::Event::Press, d.update(true, 40));
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(true, 2000));
    TEST_ASSERT_EQUAL(Debouncer::Event::LongPress, d.update(true, 2040));
    TEST_ASSERT_EQUAL(Debouncer::Event::None, d.update(true, 3000)); // fires once
    d.update(false, 3100);
    TEST_ASSERT_EQUAL(Debouncer::Event::Release, d.update(false, 3140));
    TEST_ASSERT_FALSE(d.wasShortPress());
}

// Air-raid colour change: END of one colour while the other stays active is
// voiced Silent by main.cpp - state must still clear, no END pattern.
void test_silent_end_is_bookkeeping_only() {
    NotificationEngine n;
    n.onEngineEvent({AlertEvent::Started, AlertType::AirRaid},
                    NotificationEngine::StartupMode::Silent, 0);
    n.onEngineEvent({AlertEvent::Ended, AlertType::AirRaid},
                    NotificationEngine::StartupMode::Silent, 1000);
    TEST_ASSERT_EQUAL(0, n.queued());
    TEST_ASSERT_FALSE(n.tick(2000));
    n.onEngineEvent({AlertEvent::Started, AlertType::AirRaid},
                    NotificationEngine::StartupMode::Silent, 3000);
    n.onEngineEvent({AlertEvent::Ended, AlertType::AirRaid},
                    NotificationEngine::StartupMode::Normal, 4000);
    TEST_ASSERT_EQUAL(1, n.queued()); // Normal END still sounds
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_start_plays_start_pattern);
    RUN_TEST(test_silent_start_makes_no_sound_but_end_does);
    RUN_TEST(test_mute_silences_immediately);
    RUN_TEST(test_new_type_overrides_mute);
    RUN_TEST(test_muted_type_stays_silent_until_clear);
    RUN_TEST(test_manual_test_sounds_when_muted);
    RUN_TEST(test_manual_test_uses_configured_duration);
    RUN_TEST(test_escalation_policy_can_disable_signal);
    RUN_TEST(test_additional_location_policy_can_enable_signal);
    RUN_TEST(test_stale_data_suppresses_reminders_by_default);
    RUN_TEST(test_stale_reminders_can_be_enabled);
    RUN_TEST(test_end_does_not_preempt_start);
    RUN_TEST(test_start_preempts_reminder);
    RUN_TEST(test_reminder_interval);
    RUN_TEST(test_disabled_profile_no_sound);
    RUN_TEST(test_silent_end_is_bookkeeping_only);
    RUN_TEST(test_debouncer_press_release);
    RUN_TEST(test_debouncer_bounce_ignored);
    RUN_TEST(test_debouncer_long_press);
    return UNITY_END();
}
