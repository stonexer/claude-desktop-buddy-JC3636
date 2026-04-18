#pragma once
#include <stdint.h>

class Canvas;

// Must be called once after Canvas is constructed; reads the canvas
// width and centers the blob. Safe to call again if canvas is resized.
void blobInit(Canvas& c);

// Clear the canvas to the blob background and render one frame of the
// requested state. `tick` is a monotonic counter that drives the
// per-state animation sequences; callers typically bump it every
// TICK_MS (~200 ms).
//
// state uses the PersonaState enum: 0=sleep, 1=idle, 2=busy,
// 3=attention, 4=celebrate, 5=dizzy, 6=heart. Out-of-range falls back
// to idle.
void blobRender(Canvas& c, uint32_t tick, uint8_t state);
