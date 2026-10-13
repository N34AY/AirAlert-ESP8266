#pragma once
#include "Pattern.h"
#include "Types.h"

namespace airalert {

// Per-threat siren profile (SPEC 45-46). Values are user-editable via
// Web UI later (Phase 6); these are the shipped defaults.
struct AlertProfile {
    bool enabled = true;
    Pattern start = Pattern::pulses(3000, 1000, 3);
    Pattern end = Pattern::pulses(1000, 1000, 2);
    Pattern reminder = Pattern::pulses(1000, 1000, 2, false);
    uint32_t reminderIntervalMs = 900000; // 15 min
    uint8_t priority = 50;
};

// Default priority order per SPEC 49: nuclear > chemical > artillery > urban >
// air raid (red) > air raid (yellow). Yellow = drone threat: shorter start
// pattern by default; every value is user-editable per profile.
inline AlertProfile defaultProfile(AlertType t) {
    AlertProfile p;
    switch (t) {
        case AlertType::Nuclear: p.priority = 90; break;
        case AlertType::Chemical: p.priority = 80; break;
        case AlertType::ArtilleryShelling: p.priority = 60; break;
        case AlertType::UrbanFights: p.priority = 55; break;
        case AlertType::AirRaid: p.priority = 50; break;
        case AlertType::AirRaidYellow:
            p.priority = 45;
            p.start = Pattern::pulses(1000, 1000, 2);
            break;
        default: p.priority = 40; break; // Unknown: signal, lowest priority
    }
    return p;
}

// SPEC 54: manual TEST -> 500-1000 ms single relay pulse.
inline Pattern manualTestPattern() { return Pattern::pulses(600, 0, 1); }

} // namespace airalert
