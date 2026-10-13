#pragma once
#include <cstdint>
#include "TimeUtils.h"

namespace airalert {

// SPEC 44. A siren signal is a sequence of steps (siren ON or pause), played
// `repeat` times in a row. Steps are stored in 10 ms units, sign = kind:
// > 0 siren ON, < 0 pause. Packed so AppConfig stays small enough for the
// handlers that copy it onto the 4 KB ESP8266 stack.
struct Pattern {
    static constexpr uint8_t kMaxSteps = 12;
    static constexpr uint32_t kMaxStepMs = 60000;
    static constexpr uint32_t kUnitMs = 10;

    bool enabled = true;
    uint8_t repeat = 1; // whole-sequence repetitions
    uint8_t count = 0;
    int16_t steps[kMaxSteps] = {};

    // Classic on/off/repeat signal: ON, pause, ON, pause, ... ON.
    static Pattern pulses(uint32_t onMs, uint32_t offMs, uint8_t repeat, bool enabled = true) {
        Pattern p;
        p.enabled = enabled;
        p.repeat = repeat;
        p.add(true, onMs);
        p.add(false, offMs);
        return p;
    }

    // Appends a step; ms is rounded to the 10 ms unit, 0 adds nothing.
    bool add(bool on, uint32_t ms) {
        if (ms > kMaxStepMs) return false;
        const int16_t units = static_cast<int16_t>((ms + kUnitMs / 2) / kUnitMs);
        if (units == 0) return ms == 0;
        if (count >= kMaxSteps) return false;
        steps[count++] = on ? units : static_cast<int16_t>(-units);
        return true;
    }

    bool stepOn(uint8_t i) const { return steps[i] > 0; }
    uint32_t stepMs(uint8_t i) const {
        return static_cast<uint32_t>(steps[i] > 0 ? steps[i] : -steps[i]) * kUnitMs;
    }
    bool hasOn() const {
        for (uint8_t i = 0; i < count; ++i)
            if (stepOn(i)) return true;
        return false;
    }
    // First step of the kind, 0 if none (legacy on_ms/off_ms view).
    uint32_t firstMs(bool on) const {
        for (uint8_t i = 0; i < count; ++i)
            if (stepOn(i) == on) return stepMs(i);
        return 0;
    }
};

// Non-blocking single-pattern playback (SPEC 43). Pure logic, millis-driven.
class PatternScheduler {
public:
    void start(const Pattern& p, uint32_t nowMs) {
        pattern_ = p;
        rep_ = 0;
        idx_ = 0;
        phaseStart_ = nowMs;
        running_ = p.enabled && p.repeat > 0 && p.hasOn();
        skipSilence();
    }

    void stop() { running_ = false; }

    // Advance; returns desired output state (true = relay/LED ON).
    bool tick(uint32_t nowMs) {
        // Phase boundaries advance by NOMINAL durations, not by tick time,
        // so a coarse tick interval cannot stretch the pattern.
        while (running_ && intervalPassed(nowMs, phaseStart_, pattern_.stepMs(idx_))) {
            phaseStart_ += pattern_.stepMs(idx_);
            if (++idx_ >= pattern_.count) {
                idx_ = 0;
                ++rep_;
            }
            skipSilence();
        }
        return running_ && pattern_.stepOn(idx_);
    }

    bool running() const { return running_; }

private:
    // The signal is over once no ON step is left: trailing pauses of the
    // last repetition are not waited out.
    void skipSilence() {
        if (!running_) return;
        if (rep_ >= pattern_.repeat) { running_ = false; return; }
        if (rep_ + 1 < pattern_.repeat) return; // a later repetition has ON
        for (uint8_t i = idx_; i < pattern_.count; ++i)
            if (pattern_.stepOn(i)) return;
        running_ = false;
    }

    Pattern pattern_{};
    uint32_t phaseStart_ = 0;
    uint8_t idx_ = 0;
    uint8_t rep_ = 0;
    bool running_ = false;
};

} // namespace airalert
