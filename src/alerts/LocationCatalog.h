#pragma once
// ILocationCatalog over the generated PROGMEM table (SPEC 13, 16).
#include <pgmspace.h>
#include <cstring>
#include "airalert/Location.h"
#include "LocationTable.h"

// LocationRow::type is LocationType by value; the generator hard-codes the
// order (tools/update_locations.py TYPE_ENUM), so a reordered enum would
// silently mis-type every row.
static_assert(static_cast<uint8_t>(airalert::LocationType::Oblast) == 0 &&
              static_cast<uint8_t>(airalert::LocationType::Raion) == 1 &&
              static_cast<uint8_t>(airalert::LocationType::Hromada) == 2 &&
              static_cast<uint8_t>(airalert::LocationType::City) == 3,
              "LocationType order must match tools/update_locations.py TYPE_ENUM");

class LocationCatalog : public airalert::ILocationCatalog {
public:
    bool findByUid(uint16_t uid, airalert::Location& out) const override {
        // table is sorted by uid -> binary search; rows live in flash, so
        // every access goes through memcpy_P (no direct struct reads)
        LocationRow r;
        size_t lo = 0, hi = kLocationTableSize;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            memcpy_P(&r, &kLocationTable[mid], sizeof r);
            if (r.uid < uid) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= kLocationTableSize) return false;
        memcpy_P(&r, &kLocationTable[lo], sizeof r);
        if (r.uid != uid) return false;
        out.uid = r.uid;
        out.type = static_cast<airalert::LocationType>(r.type);
        out.oblastUid = r.oblastUid;
        out.raionUid = r.raionUid;
        return true;
    }
};
