#pragma once
#include <cstdint>
#include "AlertEngine.h"
#include "Types.h"

namespace airalert {

// SPEC 26: alert types already present in the first snapshot after boot (or
// after a location change) started before the device was watching. Their
// Started event is applied silently - the siren only answers transitions
// that happen while the device is running. A type is forgotten once it is
// absent from a snapshot, so its next appearance is a real start.
// Tracked per type rather than "first snapshot only" so start_conf > 1,
// where Started arrives a few polls later, is covered too.
class PreexistingAlerts {
public:
    // Call with every snapshot BEFORE AlertEngine::applySnapshot; `firstSync`
    // is !engine.synced(). `partialActive` mirrors the engine rule (SPEC 20).
    void observe(const AlertEngine::Snapshot& s, bool firstSync, bool partialActive) {
        if (firstSync) mask_ = 0;
        for (uint8_t i = 0; i < kAlertTypeCount; ++i) {
            const Coverage cov = s.types[i].coverage;
            const bool present =
                cov == Coverage::Full || (cov == Coverage::Partial && partialActive);
            if (!present) mask_ &= static_cast<uint8_t>(~(1u << i));
            else if (firstSync) mask_ |= static_cast<uint8_t>(1u << i);
        }
    }

    // For a Started event: true if it must stay silent. One-shot per type.
    bool consumeStart(AlertType t) {
        const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(t));
        const bool hit = mask_ & bit;
        mask_ &= static_cast<uint8_t>(~bit);
        return hit;
    }

private:
    uint8_t mask_ = 0; // bit per AlertType
};

} // namespace airalert
