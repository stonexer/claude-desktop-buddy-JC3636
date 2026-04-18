#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TouchTap {
  int16_t x;  // 0..359 screen coordinates
  int16_t y;
};

enum TouchEvent {
  TOUCH_NONE = 0,
  TOUCH_TAP,          // quick press + release, no swipe
  TOUCH_SWIPE_LEFT,
  TOUCH_SWIPE_RIGHT,
  TOUCH_SWIPE_UP,
  TOUCH_SWIPE_DOWN,
};

// Initialize Wire on the touch bus and reset the CST816S.
bool touch_init(void);

// DEPRECATED: press-edge tap only. Kept for back-compat — prefer
// touch_poll_event(), which distinguishes taps from swipes.
bool touch_poll_tap(struct TouchTap* out);

// Release-edge event classifier. Returns true exactly on the frame the
// finger lifts, reporting the gesture kind in *evt and the lift
// position in *pos. Uses the CST816S gesture register when populated
// (vendor firmware fills it on swipe) and falls back to a simple
// start→end vector threshold otherwise.
bool touch_poll_event(enum TouchEvent* evt, struct TouchTap* pos);

#ifdef __cplusplus
}
#endif
