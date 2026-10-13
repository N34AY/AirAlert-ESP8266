#pragma once
#include <cstdint>
#include "AlertEngine.h"
#include "AlertProfiles.h"
#include "MuteState.h"
#include "NotificationQueue.h"
#include "Pattern.h"

namespace airalert {

// Glues queue + scheduler + mute + profiles into one non-blocking siren
// policy (SPEC 34, 47-54). Pure: output is the bool returned by tick();
// the firmware feeds it through RelayGuard into RelayController.
class NotificationEngine {
public:
    struct Config {
        bool notifyEscalation = true;
        bool notifyAdditionalLocation = false;
        bool remindersWhenStale = false;
        uint16_t manualTestMs = 600;
    };

    NotificationEngine() {
        for (uint8_t i = 0; i < kAlertTypeCount; ++i)
            profiles_[i] = defaultProfile(static_cast<AlertType>(i));
    }

    void setProfile(AlertType t, const AlertProfile& p) {
        profiles_[static_cast<uint8_t>(t)] = p;
    }
    void setMuteConfig(const MuteState::Config& c) { mute_.setConfig(c); }
    void setConfig(const Config& c) { cfg_ = c; }
    void setApiStale(bool stale) { apiStale_ = stale; }

    // How a Started/Ended event is voiced:
    //   Normal - full pattern; Silent - state bookkeeping only, no sound
    //   (alert already active at boot - SPEC 26, partial without siren,
    //   air-raid colour change).
    enum class StartupMode : uint8_t { Normal, Silent };
    void onEngineEvent(const EngineEvent& ev, StartupMode mode, uint32_t nowMs);

    void muteShort(uint32_t nowMs) { doMute(MuteState::Scope::UntilClear, nowMs); }
    void muteLong(uint32_t nowMs) { doMute(MuteState::Scope::Snooze, nowMs); }
    void unmute() { mute_.unmute(); }
    void manualTest(uint32_t nowMs) {
        playTest(Pattern::pulses(cfg_.manualTestMs, 0, 1), nowMs);
    }
    // Plays an arbitrary pattern as a manual test (Web UI sequence preview).
    void playTest(const Pattern& p, uint32_t nowMs) {
        testPattern_ = p;
        queue_.push({Signal::ManualTest, AlertType::Unknown, 255});
        (void)nowMs;
    }

    // Advance; returns desired siren output (pre-RelayGuard).
    bool tick(uint32_t nowMs);

    bool muted() const { return mute_.muted(); }
    bool playing() const { return playing_; }
    size_t queued() const { return queue_.size(); }

    // Invariant 8: OTA must stop everything.
    void stopAll() {
        sched_.stop();
        queue_.clear();
        playing_ = false;
    }
    void resetAlertState() {
        stopAll();
        activeMask_ = 0;
        for (auto& at : lastReminderAt_) at = 0;
        mute_.onAllClear();
    }

private:
    void doMute(MuteState::Scope scope, uint32_t nowMs) {
        // Invariant 4: silence immediately; caller also forces relay OFF.
        sched_.stop();
        queue_.clear();
        playing_ = false;
        mute_.mute(scope, nowMs, activeMask_);
    }

    const AlertProfile& prof(AlertType t) const {
        return profiles_[static_cast<uint8_t>(t)];
    }
    Pattern patternFor(const AlertProfile& p, Signal s) const {
        switch (s) {
            case Signal::Start:
            case Signal::Escalation: return p.start; // SPEC 35
            case Signal::End: return p.end;
            case Signal::Reminder: return p.reminder;
            case Signal::ManualTest: return testPattern_;
        }
        return Pattern{};
    }

    AlertProfile profiles_[kAlertTypeCount];
    Config cfg_;
    Pattern testPattern_ = manualTestPattern();
    NotificationQueue queue_;
    PatternScheduler sched_;
    MuteState mute_;
    Notification current_{};
    bool playing_ = false;
    bool apiStale_ = false;

    uint8_t activeMask_ = 0; // bit per active AlertType
    uint32_t lastReminderAt_[kAlertTypeCount] = {};
};

inline void NotificationEngine::onEngineEvent(const EngineEvent& ev, StartupMode mode,
                                              uint32_t nowMs) {
    const uint8_t bit = 1u << static_cast<uint8_t>(ev.type);
    const AlertProfile& p = prof(ev.type);
    switch (ev.kind) {
        case AlertEvent::Started:
            activeMask_ |= bit;
            lastReminderAt_[static_cast<uint8_t>(ev.type)] = nowMs;
            if (!p.enabled || mode == StartupMode::Silent) break; // SPEC 26
            queue_.push({Signal::Start, ev.type, p.priority});
            // SPEC 47: Start may preempt a playing Reminder
            if (playing_ && NotificationQueue::canPreempt(
                    {Signal::Start, ev.type, p.priority}, current_)) {
                sched_.stop();
                playing_ = false;
            }
            break;
        case AlertEvent::Escalated: // SPEC 35
            if (p.enabled && cfg_.notifyEscalation)
                queue_.push({Signal::Escalation, ev.type, p.priority});
            break;
        case AlertEvent::Ended:
            activeMask_ &= static_cast<uint8_t>(~bit);
            if (p.enabled && mode != StartupMode::Silent)
                queue_.push({Signal::End, ev.type, p.priority});
            if (activeMask_ == 0) mute_.onAllClear(); // SPEC 52
            break;
        case AlertEvent::LocationAdded: // SPEC 37
            if (p.enabled && cfg_.notifyAdditionalLocation)
                queue_.push({Signal::Escalation, ev.type, p.priority});
            break;
        case AlertEvent::CoverageReduced: // SPEC 36: no signal
        default:
            break;
    }
}

inline bool NotificationEngine::tick(uint32_t nowMs) {
    // Self-scheduled reminders while a type stays active (SPEC 34, 44).
    for (uint8_t i = 0; i < kAlertTypeCount; ++i) {
        const AlertProfile& p = profiles_[i];
        if ((activeMask_ & (1u << i)) && p.enabled && p.reminder.enabled &&
            (!apiStale_ || cfg_.remindersWhenStale) &&
            intervalPassed(nowMs, lastReminderAt_[i], p.reminderIntervalMs)) {
            lastReminderAt_[i] = nowMs;
            queue_.push({Signal::Reminder, static_cast<AlertType>(i), p.priority});
        }
    }

    if (playing_) {
        const bool out = sched_.tick(nowMs);
        if (!sched_.running()) {
            playing_ = false;
            mute_.onPatternFinished();
        }
        return out;
    }

    Notification n;
    while (queue_.pop(n)) {
        // ManualTest always sounds - explicit user action (SPEC 54).
        if (n.signal != Signal::ManualTest && mute_.shouldSilence(n.type, nowMs))
            continue; // silenced; try next queued item
        current_ = n;
        sched_.start(patternFor(prof(n.type), n.signal), nowMs);
        if (sched_.running()) {
            playing_ = true;
            return sched_.tick(nowMs);
        }
    }
    return false;
}

} // namespace airalert
