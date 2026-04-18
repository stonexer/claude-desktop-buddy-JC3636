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

// Owner name — sent by the desktop as {"cmd":"owner","name":"Felix"}.
// Persisted so the pet greets them by name after a power cycle.
// ownerLoad always null-terminates `out`, even on a fresh NVS.
inline void ownerLoad(char* out, size_t cap) {
  if (!out || cap == 0) return;
  _buddyPrefs.begin("buddy", /*readOnly=*/true);
  _buddyPrefs.getString("owner", out, cap);
  _buddyPrefs.end();
  out[cap - 1] = 0;
}

inline void ownerSave(const char* name) {
  if (!name) return;
  _buddyPrefs.begin("buddy", /*readOnly=*/false);
  _buddyPrefs.putString("owner", name);
  _buddyPrefs.end();
}

// Level — one unit per 50 000 output tokens the desktop reports. Only
// written to NVS when it changes (≈ once every few hours of active
// use), so the flash wear budget is fine.
inline uint8_t levelLoad() {
  _buddyPrefs.begin("buddy", /*readOnly=*/true);
  uint8_t v = _buddyPrefs.getUChar("level", 0);
  _buddyPrefs.end();
  return v;
}

inline void levelSave(uint8_t lvl) {
  _buddyPrefs.begin("buddy", /*readOnly=*/false);
  _buddyPrefs.putUChar("level", lvl);
  _buddyPrefs.end();
}
