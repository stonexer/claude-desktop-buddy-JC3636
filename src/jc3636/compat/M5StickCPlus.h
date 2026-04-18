// Compatibility shim so the unmodified src/buddies/*.cpp can be built
// for JC3636W518EN. Those files do `#include <M5StickCPlus.h>` and
// reference TFT_eSprite / TFT_eSPI types; we redirect them to our
// Canvas class. All species call helpers (buddyPrintLine,
// buddyPrintSprite, buddySetCursor, ...) from buddy_common.h — those
// are implemented in src/jc3636/buddy_mux.cpp against Canvas, so the
// species files never see M5 internals.

#pragma once

#include "canvas.h"

// Aliased via preprocessor (not typedef) because buddy.h contains a
// `class TFT_eSPI;` forward declaration — with a typedef that would
// collide with "using a class-key on a typedef name".
#define TFT_eSprite Canvas
#define TFT_eSPI    Canvas

// The M5 code uses these text-datum constants; the JC3636 canvas only
// supports top-left (cursor-based) rendering, so these values are
// ignored — we define them so the expressions still compile when a
// species happens to use them (none in the current buddies do).
#ifndef TL_DATUM
#define TL_DATUM 0
#define TR_DATUM 1
#define BL_DATUM 2
#define BR_DATUM 3
#define ML_DATUM 4
#define MC_DATUM 5
#define MR_DATUM 6
#endif
