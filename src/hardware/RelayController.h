#pragma once
// Sole owner of the siren relay GPIOs (SPEC 38-42, 172). Everything routes
// through RelayGuard, so no caller can hold the siren ON past the safety limit.
//
// Two-relay module wired in parallel with the siren controller's buttons:
// START relay (D5) and STOP relay (D6). The siren latches, so a state change is
// a short "button press" pulse on the matching relay, never a held level.
// on_ is the latched siren state as we last commanded it.
#include <Arduino.h>
#include <Ticker.h>
#include "airalert/RelayGuard.h"

class RelayController {
public:
    static constexpr uint32_t kPulseMs = 300; // simulated button press

    void begin(uint8_t startPin, uint8_t stopPin, bool activeHigh, uint32_t maxOnMs) {
        safetyTimer_.detach();
        pulseTimer_.detach();
        // begin() re-runs on every config save, possibly mid-pattern: a siren
        // latched ON must be stopped, or it would howl with no deadline armed.
        const bool wasOn = on_;
        if (startPin_ != 255) { // release pins with the OLD polarity first
            digitalWrite(startPin_, activeHigh_ ? LOW : HIGH);
            digitalWrite(stopPin_, activeHigh_ ? LOW : HIGH);
        }
        pulsePin_ = 255;
        startPin_ = startPin;
        stopPin_ = stopPin;
        activeHigh_ = activeHigh;
        maxOnMs_ = maxOnMs;
        asyncTrip_ = false;
        asyncOffNeedsLog_ = false;
        tripEvent_ = false;
        guard_.tick(false, 0);
        guard_.setLimit(maxOnMs);
        release(startPin_);
        release(stopPin_);
        pinMode(startPin_, OUTPUT);
        pinMode(stopPin_, OUTPUT);
        release(startPin_);
        release(stopPin_);
        // The cached state must say OFF: a stale on_=true would make the next
        // apply(true) a no-op - siren dead AND the safety deadline never armed.
        on_ = false;
        if (wasOn) pulse(stopPin_);
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
    void forceOff() {
        safetyTimer_.detach();
        asyncTrip_ = false;
        asyncOffNeedsLog_ = false;
        tripEvent_ = false;
        guard_.tick(false, 0); // release guard state
        on_ = false;
        if (pulsePin_ != stopPin_) pulse(stopPin_); // preempts a START press
        Serial.println("[RELAY] STOP (forced)");
    }

    // Commissioning: press ONE button by hand (SPEC 54). START arms the
    // safety deadline, so a siren started this way still stops on its own.
    void testPulse(bool start) {
        pulse(start ? startPin_ : stopPin_);
        if (start)
            safetyTimer_.once_ms(maxOnMs_, safetyTimeout, this);
        else if (!on_) // a playing pattern keeps its own deadline
            safetyTimer_.detach();
        Serial.printf("[RELAY] TEST %s pulse\n", start ? "START" : "STOP");
    }

    bool isOn() const { return on_; }
    bool safetyTripped() const { return asyncTrip_ || guard_.tripped(); }
    bool consumeSafetyTrip() {
        const bool event = tripEvent_;
        tripEvent_ = false;
        return event;
    }

private:
    static void safetyTimeout(RelayController* self) { self->emergencyOff(); }
    static void pulseDone(RelayController* self) { self->endPulse(); }

    // SYS-context deadline: it remains effective while loop() is blocked.
    void emergencyOff() {
        on_ = false;
        if (pulsePin_ != stopPin_) pulse(stopPin_);
        asyncTrip_ = true;
        asyncOffNeedsLog_ = true;
        tripEvent_ = true;
    }

    void apply(bool on) {
        if (on == on_) return;
        // one press at a time: the change is retried on the next tick
        if (pulsePin_ != 255) return;
        on_ = on;
        pulse(on ? startPin_ : stopPin_);
        if (on)
            safetyTimer_.once_ms(maxOnMs_, safetyTimeout, this);
        else
            safetyTimer_.detach();
        // commissioning aid (SPEC 141): every physical press is visible
        Serial.printf("[RELAY] %s\n", on ? "START" : "STOP");
    }

    // Press a button; the release runs from a timer so loop() never blocks.
    void pulse(uint8_t pin) {
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

    airalert::RelayGuard guard_;
    Ticker safetyTimer_;
    Ticker pulseTimer_;
    uint8_t startPin_ = 255;
    uint8_t stopPin_ = 255;
    volatile uint8_t pulsePin_ = 255;
    uint32_t maxOnMs_ = 30000;
    bool activeHigh_ = true;
    volatile bool on_ = false;
    volatile bool asyncTrip_ = false;
    volatile bool asyncOffNeedsLog_ = false;
    volatile bool tripEvent_ = false;
};
