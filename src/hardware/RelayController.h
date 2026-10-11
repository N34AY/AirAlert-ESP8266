#pragma once
// Sole owner of the siren relay GPIOs (SPEC 38-42, 172). Everything routes
// through RelayGuard, so no caller can hold the siren ON past the safety limit.
//
// Two wirings, picked by config (relay.single):
// - pulse (two relays, default): wired in parallel with the siren controller's
//   buttons, START relay (D5) and STOP relay (D6). The siren latches, so a
//   state change is a short "button press" pulse on the matching relay.
// - level (one relay): D5 switches the siren directly and is held for the
//   whole ON phase; D6 is left alone.
// on_ is the siren state as we last commanded it.
//
// Every press is queued as a Press so loop() can write it to the event log:
// presses also come from Ticker callbacks, where flash I/O is not allowed.
// Ticker (os_timer) callbacks never preempt loop() mid-statement on ESP8266,
// so the single-slot-per-press ring needs no locking.
#include <Arduino.h>
#include <Ticker.h>
#include "airalert/RelayGuard.h"

class RelayController {
public:
    static constexpr uint32_t kPulseMs = 300; // simulated button press

    struct Press {
        bool start;      // START press (level mode: ON), else STOP (OFF)
        const char* why; // string literal: pattern, mute, safety, test, ...
    };

    void begin(uint8_t startPin, uint8_t stopPin, bool activeHigh, uint32_t maxOnMs,
               bool level) {
        safetyTimer_.detach();
        pulseTimer_.detach();
        // begin() re-runs on every config save, possibly mid-pattern: a siren
        // latched ON must be stopped, or it would howl with no deadline armed.
        const bool wasOn = on_;
        if (startPin_ != 255) { // release pins with the OLD polarity and mode first
            release(startPin_);
            if (!level_) release(stopPin_);
        }
        pulsePin_ = 255;
        startPin_ = startPin;
        stopPin_ = stopPin;
        activeHigh_ = activeHigh;
        level_ = level;
        maxOnMs_ = maxOnMs;
        asyncTrip_ = false;
        asyncOffNeedsLog_ = false;
        tripEvent_ = false;
        guard_.tick(false, 0);
        guard_.setLimit(maxOnMs);
        release(startPin_);
        pinMode(startPin_, OUTPUT);
        release(startPin_);
        if (!level_) {
            release(stopPin_);
            pinMode(stopPin_, OUTPUT);
            release(stopPin_);
        } else {
            pinMode(stopPin_, INPUT); // unused: stop driving a leftover STOP relay
        }
        // The cached state must say OFF: a stale on_=true would make the next
        // apply(true) a no-op - siren dead AND the safety deadline never armed.
        on_ = false;
        if (wasOn) press(false, "reconfig"); // level mode: already released above
    }

    // Feed the desired state every loop; the guard decides what the siren gets.
    void tick(bool requested, uint32_t nowMs) {
        const bool trippedBefore = guard_.tripped();
        if (asyncTrip_) guard_.trip();
        apply(guard_.tick(requested, nowMs));
        if (!trippedBefore && guard_.tripped()) tripEvent_ = true;
        if (!requested) asyncTrip_ = false;
        if (asyncOffNeedsLog_) {
            asyncOffNeedsLog_ = false;
            Serial.println("[RELAY] STOP (async safety limit)");
        }
    }

    // SPEC 40. Always presses STOP, even if we think the siren is already
    // off: the latched state may have been changed by hand on the siren itself.
    void forceOff(const char* why) {
        safetyTimer_.detach();
        asyncTrip_ = false;
        asyncOffNeedsLog_ = false;
        tripEvent_ = false;
        guard_.tick(false, 0); // release guard state
        on_ = false;
        if (level_ || pulsePin_ != stopPin_) press(false, why); // preempts a START press
        Serial.printf("[RELAY] STOP (forced: %s)\n", why);
    }

    // Commissioning: press ONE button by hand (SPEC 54). START arms the
    // safety deadline, so a siren started this way still stops on its own.
    void testPulse(bool start) {
        press(start, "test");
        if (start)
            safetyTimer_.once_ms(maxOnMs_, safetyTimeout, this);
        else if (!on_) // a playing pattern keeps its own deadline
            safetyTimer_.detach();
        Serial.printf("[RELAY] TEST %s\n", start ? "START" : "STOP");
    }

    bool isOn() const { return on_; }
    bool levelMode() const { return level_; }
    bool safetyTripped() const { return asyncTrip_ || guard_.tripped(); }
    bool consumeSafetyTrip() {
        const bool event = tripEvent_;
        tripEvent_ = false;
        return event;
    }

    // Oldest press not yet logged; dropped counts presses lost to overflow.
    bool popPress(Press& out, uint8_t& dropped) {
        dropped = dropped_;
        dropped_ = 0;
        if (pressTail_ == pressHead_) return false;
        out = presses_[pressTail_];
        pressTail_ = (pressTail_ + 1) % kPressQueue;
        return true;
    }

private:
    static void safetyTimeout(RelayController* self) { self->emergencyOff(); }
    static void pulseDone(RelayController* self) { self->endPulse(); }

    // SYS-context deadline: it remains effective while loop() is blocked.
    void emergencyOff() {
        on_ = false;
        if (level_ || pulsePin_ != stopPin_) press(false, "safety");
        asyncTrip_ = true;
        asyncOffNeedsLog_ = true;
        tripEvent_ = true;
    }

    void apply(bool on) {
        if (on == on_) return;
        // one press at a time: the change is retried on the next tick
        if (pulsePin_ != 255) return;
        on_ = on;
        press(on, "pattern");
        if (on)
            safetyTimer_.once_ms(maxOnMs_, safetyTimeout, this);
        else
            safetyTimer_.detach();
        // commissioning aid (SPEC 141): every physical press is visible
        Serial.printf("[RELAY] %s\n", on ? "START" : "STOP");
    }

    // Level mode holds D5 for as long as the siren should sound; pulse mode
    // taps the matching button.
    void press(bool start, const char* why) {
        if (!level_) return pulse(start ? startPin_ : stopPin_, why);
        queuePress({start, why});
        digitalWrite(startPin_, start == activeHigh_ ? HIGH : LOW);
    }

    // Press a button; the release runs from a timer so loop() never blocks.
    void pulse(uint8_t pin, const char* why) {
        queuePress({pin == startPin_, why});
        pulseTimer_.detach();
        if (pulsePin_ != 255) release(pulsePin_); // never both pressed at once
        pulsePin_ = pin;
        digitalWrite(pin, activeHigh_ ? HIGH : LOW);
        pulseTimer_.once_ms(kPulseMs, pulseDone, this);
    }
    void endPulse() {
        if (pulsePin_ != 255) release(pulsePin_);
        pulsePin_ = 255;
    }
    void release(uint8_t pin) { digitalWrite(pin, activeHigh_ ? LOW : HIGH); }
    void queuePress(const Press& p) {
        const uint8_t next = (pressHead_ + 1) % kPressQueue;
        if (next == pressTail_) { // full: keep the oldest, count the loss
            if (dropped_ < 255) ++dropped_;
            return;
        }
        presses_[pressHead_] = p;
        pressHead_ = next;
    }

    static constexpr uint8_t kPressQueue = 16;
    Press presses_[kPressQueue];
    volatile uint8_t pressHead_ = 0, pressTail_ = 0, dropped_ = 0;

    airalert::RelayGuard guard_;
    Ticker safetyTimer_;
    Ticker pulseTimer_;
    uint8_t startPin_ = 255;
    uint8_t stopPin_ = 255;
    volatile uint8_t pulsePin_ = 255;
    uint32_t maxOnMs_ = 30000;
    bool activeHigh_ = true;
    bool level_ = false;
    volatile bool on_ = false;
    volatile bool asyncTrip_ = false;
    volatile bool asyncOffNeedsLog_ = false;
    volatile bool tripEvent_ = false;
};
