// Minimal stats shim for the JC3636 build. Only provides what
// buddy.cpp in the shared tree requires (speciesIdx load/save); the
// M5 firmware's full stats.h is out of scope here — battery, tokens,
// approvals etc. aren't piped through this port yet.
//
// speciesIdx lives in NVS under "buddy" namespace. Reads are cheap
// (a few hundred µs) so callers can invoke them during state changes
// without caching.

#pragma once

#include <stdint.h>
#include <Preferences.h>

static Preferences _buddyPrefs;

inline uint8_t speciesIdxLoad() {
  _buddyPrefs.begin("buddy", /*readOnly=*/true);
  uint8_t v = _buddyPrefs.getUChar("species", 0);
  _buddyPrefs.end();
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  _buddyPrefs.begin("buddy", /*readOnly=*/false);
  _buddyPrefs.putUChar("species", idx);
  _buddyPrefs.end();
}
