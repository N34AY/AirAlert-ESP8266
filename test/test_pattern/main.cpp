// PatternScheduler / RelayGuard / NotificationQueue / MuteState — SPEC 137
#include <unity.h>
#include "airalert/Pattern.h"
#include "airalert/RelayGuard.h"
#include "airalert/NotificationQueue.h"
#include "airalert/MuteState.h"

using namespace airalert;

void test_single_pulse() {
    PatternScheduler p;
    p.start(Pattern::pulses(1000, 0, 1), 0);
    TEST_ASSERT_TRUE(p.tick(0));
    TEST_ASSERT_TRUE(p.tick(999));
    TEST_ASSERT_FALSE(p.tick(1000));
    TEST_ASSERT_FALSE(p.running());
}

void test_multiple_repeats() {
    PatternScheduler p;
    p.start(Pattern::pulses(100, 50, 3), 0); // ON 0-100, OFF 100-150, ON 150-250 ...
    TEST_ASSERT_TRUE(p.tick(50));
    TEST_ASSERT_FALSE(p.tick(120));
    TEST_ASSERT_TRUE(p.tick(160));
    TEST_ASSERT_FALSE(p.tick(260));
    TEST_ASSERT_TRUE(p.tick(320));
    TEST_ASSERT_FALSE(p.tick(430));
    TEST_ASSERT_FALSE(p.running());
}

void test_zero_off_time() {
    PatternScheduler p;
    p.start(Pattern::pulses(100, 0, 3), 0);
    TEST_ASSERT_TRUE(p.tick(150));  // chains straight into next ON
    TEST_ASSERT_TRUE(p.tick(250));
    TEST_ASSERT_FALSE(p.tick(350)); // 3 repeats done
}

void test_disabled_or_zero() {
    PatternScheduler p;
    p.start(Pattern::pulses(1000, 0, 1, false), 0);
    TEST_ASSERT_FALSE(p.tick(10));
    p.start(Pattern::pulses(0, 0, 5), 0);
    TEST_ASSERT_FALSE(p.tick(10));
    p.start(Pattern::pulses(100, 0, 0), 0);
    TEST_ASSERT_FALSE(p.tick(10));
}

void test_stop_mid_on() { // mute during ON
    PatternScheduler p;
    p.start(Pattern::pulses(1000, 100, 2), 0);
    TEST_ASSERT_TRUE(p.tick(500));
    p.stop();
    TEST_ASSERT_FALSE(p.tick(600));
    TEST_ASSERT_FALSE(p.running());
}

void test_millis_wraparound() { // SPEC 137: mandatory
    PatternScheduler p;
    const uint32_t nearWrap = 0xFFFFFF00u; // 256 ms before wrap
    p.start(Pattern::pulses(1000, 0, 1), nearWrap);
    TEST_ASSERT_TRUE(p.tick(0xFFFFFFFEu));
    TEST_ASSERT_TRUE(p.tick(500));          // wrapped, 756 ms elapsed
    TEST_ASSERT_FALSE(p.tick(nearWrap + 1000)); // 1000 ms elapsed (wrapped value)
}

void test_sequence_spin_up() { // long first ON for siren spin-up, then shorter
    Pattern seq;
    seq.add(true, 13000);
    seq.add(false, 3000);
    seq.add(true, 10000);
    PatternScheduler p;
    p.start(seq, 0);
    TEST_ASSERT_TRUE(p.tick(12999));
    TEST_ASSERT_FALSE(p.tick(13000));
    TEST_ASSERT_FALSE(p.tick(15999));
    TEST_ASSERT_TRUE(p.tick(16000));
    TEST_ASSERT_TRUE(p.tick(25999));
    TEST_ASSERT_FALSE(p.tick(26000));
    TEST_ASSERT_FALSE(p.running());
}

void test_sequence_repeat_skips_trailing_pause() {
    Pattern seq;
    seq.add(false, 500); // leading pause plays every repetition
    seq.add(true, 100);
    seq.add(false, 400); // trailing pause of the LAST repetition is skipped
    seq.repeat = 2;
    PatternScheduler p;
    p.start(seq, 0);
    TEST_ASSERT_TRUE(p.running());
    TEST_ASSERT_FALSE(p.tick(0));
    TEST_ASSERT_TRUE(p.tick(500));
    TEST_ASSERT_FALSE(p.tick(600));
    TEST_ASSERT_FALSE(p.tick(1400)); // 2nd leading pause: 1000-1500
    TEST_ASSERT_TRUE(p.tick(1500));
    TEST_ASSERT_FALSE(p.tick(1600));
    TEST_ASSERT_FALSE(p.running());
}

void test_sequence_coarse_tick_keeps_timeline() {
    Pattern seq;
    seq.add(true, 100);
    seq.add(false, 100);
    seq.add(true, 100);
    seq.add(false, 100);
    seq.add(true, 1000);
    PatternScheduler p;
    p.start(seq, 0);
    TEST_ASSERT_TRUE(p.tick(450)); // jumped over three steps at once
    TEST_ASSERT_TRUE(p.tick(1399));
    TEST_ASSERT_FALSE(p.tick(1400));
}

void test_pattern_add_limits() {
    Pattern seq;
    TEST_ASSERT_TRUE(seq.add(true, 0)); // zero adds nothing
    TEST_ASSERT_EQUAL(0, seq.count);
    TEST_ASSERT_FALSE(seq.add(true, 60001));
    TEST_ASSERT_FALSE(seq.add(true, 3)); // rounds to zero
    TEST_ASSERT_TRUE(seq.add(true, 60000));
    TEST_ASSERT_EQUAL_UINT32(60000, seq.stepMs(0));
    for (uint8_t i = 1; i < Pattern::kMaxSteps; ++i) TEST_ASSERT_TRUE(seq.add(false, 10));
    TEST_ASSERT_FALSE(seq.add(true, 10));
}

void test_relay_guard_limit() { // Invariant 2
    RelayGuard g(30000);
    TEST_ASSERT_TRUE(g.tick(true, 0));
    TEST_ASSERT_TRUE(g.tick(true, 29999));
    TEST_ASSERT_FALSE(g.tick(true, 30000)); // tripped
    TEST_ASSERT_TRUE(g.tripped());
    TEST_ASSERT_FALSE(g.tick(true, 30001)); // stays off while still requested
    TEST_ASSERT_FALSE(g.tick(false, 31000)); // request released
    TEST_ASSERT_TRUE(g.tick(true, 32000));   // fresh cycle allowed
}

void test_relay_guard_wraparound() {
    RelayGuard g(30000);
    TEST_ASSERT_TRUE(g.tick(true, 0xFFFFF000u));
    TEST_ASSERT_FALSE(g.tick(true, 0xFFFFF000u + 30000u)); // wraps, still trips
}

void test_relay_guard_async_trip_requires_request_release() {
    RelayGuard g(30000);
    TEST_ASSERT_TRUE(g.tick(true, 100));
    g.trip();
    TEST_ASSERT_TRUE(g.tripped());
    TEST_ASSERT_FALSE(g.tick(true, 101));
    TEST_ASSERT_FALSE(g.tick(false, 102));
    TEST_ASSERT_FALSE(g.tripped());
    TEST_ASSERT_TRUE(g.tick(true, 103));
}

void test_queue_priority_order() { // SPEC 47
    NotificationQueue q;
    q.push({Signal::Reminder, AlertType::AirRaid, 50});
    q.push({Signal::End, AlertType::Chemical, 80});
    q.push({Signal::Start, AlertType::AirRaid, 50});
    Notification n;
    TEST_ASSERT_TRUE(q.pop(n));
    TEST_ASSERT_EQUAL(Signal::Start, n.signal); // Start beats End beats Reminder
    TEST_ASSERT_TRUE(q.pop(n));
    TEST_ASSERT_EQUAL(Signal::End, n.signal);
    TEST_ASSERT_TRUE(q.pop(n));
    TEST_ASSERT_EQUAL(Signal::Reminder, n.signal);
    TEST_ASSERT_FALSE(q.pop(n));
}

void test_queue_profile_priority() { // SPEC 49
    NotificationQueue q;
    q.push({Signal::Start, AlertType::AirRaid, 50});
    q.push({Signal::Start, AlertType::Nuclear, 90});
    Notification n;
    q.pop(n);
    TEST_ASSERT_EQUAL(AlertType::Nuclear, n.type);
}

void test_queue_coalescing() { // SPEC 48
    NotificationQueue q;
    for (int i = 0; i < 100; ++i)
        TEST_ASSERT_TRUE(q.push({Signal::Reminder, AlertType::AirRaid, 50}));
    TEST_ASSERT_EQUAL(1, q.size());
}

void test_queue_overflow() { // SPEC 137
    NotificationQueue q;
    // 2 signals x 6 types = 12 unique (signal,type) combos fill the queue
    for (uint8_t i = 0; i < NotificationQueue::kDepth; ++i)
        TEST_ASSERT_TRUE(q.push({i < 6 ? Signal::Reminder : Signal::End,
                                 static_cast<AlertType>(i % 6),
                                 static_cast<uint8_t>(i)}));
    TEST_ASSERT_EQUAL(NotificationQueue::kDepth, q.size());
    // 13th unique combo must be rejected, not silently dropped-oldest
    TEST_ASSERT_FALSE(q.push({Signal::Start, AlertType::AirRaid, 200}));
    TEST_ASSERT_EQUAL(NotificationQueue::kDepth, q.size());
}

void test_preemption_rules() { // SPEC 47: END never interrupts START
    Notification playingStart{Signal::Start, AlertType::AirRaid, 50};
    Notification playingReminder{Signal::Reminder, AlertType::AirRaid, 50};
    Notification end{Signal::End, AlertType::Chemical, 90};
    Notification start{Signal::Start, AlertType::Chemical, 90};
    TEST_ASSERT_FALSE(NotificationQueue::canPreempt(end, playingStart));
    TEST_ASSERT_FALSE(NotificationQueue::canPreempt(end, playingReminder));
    TEST_ASSERT_TRUE(NotificationQueue::canPreempt(start, playingReminder));
    TEST_ASSERT_FALSE(NotificationQueue::canPreempt(start, playingStart));
}

void test_mute_until_clear() { // SPEC 50, 52
    MuteState m;
    const uint8_t airMask = 1u << static_cast<uint8_t>(AlertType::AirRaid);
    m.mute(MuteState::Scope::UntilClear, 0, airMask);
    TEST_ASSERT_TRUE(m.shouldSilence(AlertType::AirRaid, 1000));
    TEST_ASSERT_TRUE(m.muted());
    m.onAllClear();
    TEST_ASSERT_FALSE(m.muted());
    TEST_ASSERT_FALSE(m.shouldSilence(AlertType::AirRaid, 2000));
}

void test_mute_new_type_overrides() { // SPEC 53
    MuteState m;
    const uint8_t airMask = 1u << static_cast<uint8_t>(AlertType::AirRaid);
    m.mute(MuteState::Scope::UntilClear, 0, airMask);
    TEST_ASSERT_TRUE(m.shouldSilence(AlertType::AirRaid, 1000));
    TEST_ASSERT_FALSE(m.shouldSilence(AlertType::Chemical, 1000)); // new threat sounds
}

void test_mute_all_types_config() { // SPEC 53
    MuteState m({15u * 60u * 1000u, true});
    m.mute(MuteState::Scope::UntilClear, 0, 0x01);
    TEST_ASSERT_TRUE(m.shouldSilence(AlertType::Chemical, 1000));
}

void test_mute_snooze_expires() { // SPEC 52
    MuteState m({1000, false});
    const uint8_t airMask = 1u << static_cast<uint8_t>(AlertType::AirRaid);
    m.mute(MuteState::Scope::Snooze, 0, airMask);
    TEST_ASSERT_TRUE(m.shouldSilence(AlertType::AirRaid, 500));
    TEST_ASSERT_FALSE(m.shouldSilence(AlertType::AirRaid, 1500)); // expired
    TEST_ASSERT_FALSE(m.muted());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_single_pulse);
    RUN_TEST(test_multiple_repeats);
    RUN_TEST(test_zero_off_time);
    RUN_TEST(test_disabled_or_zero);
    RUN_TEST(test_stop_mid_on);
    RUN_TEST(test_millis_wraparound);
    RUN_TEST(test_sequence_spin_up);
    RUN_TEST(test_sequence_repeat_skips_trailing_pause);
    RUN_TEST(test_sequence_coarse_tick_keeps_timeline);
    RUN_TEST(test_pattern_add_limits);
    RUN_TEST(test_relay_guard_limit);
    RUN_TEST(test_relay_guard_wraparound);
    RUN_TEST(test_relay_guard_async_trip_requires_request_release);
    RUN_TEST(test_queue_priority_order);
    RUN_TEST(test_queue_profile_priority);
    RUN_TEST(test_queue_coalescing);
    RUN_TEST(test_queue_overflow);
    RUN_TEST(test_preemption_rules);
    RUN_TEST(test_mute_until_clear);
    RUN_TEST(test_mute_new_type_overrides);
    RUN_TEST(test_mute_all_types_config);
    RUN_TEST(test_mute_snooze_expires);
    return UNITY_END();
}
