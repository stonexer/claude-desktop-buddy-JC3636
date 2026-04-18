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

// Initialize Wire on the touch bus and reset the CST816S. Returns false
// if the I2C bus can't come up — caller should surface the failure
// (screen prompt etc.) rather than block.
bool touch_init(void);

// Edge-triggered single-tap. Returns true exactly on the frame the
// finger first presses down (press-edge), writing the position to *out.
// While the finger is held or released, returns false. Safe to call
// every loop iteration.
bool touch_poll_tap(struct TouchTap* out);

#ifdef __cplusplus
}
#endif
