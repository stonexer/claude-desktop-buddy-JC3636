#include "touch.h"
#include "pincfg.h"
#include <Arduino.h>
#include <Wire.h>

#define CST816S_I2C_ADDR 0x15

// CST816S gesture register values (from the vendor spec — some
// rebadged panels ship with register 0x01 hardwired to 0, so we also
// fall back to a start→end vector classifier below).
#define CST_GEST_SLIDE_UP    0x01
#define CST_GEST_SLIDE_DOWN  0x02
#define CST_GEST_SLIDE_LEFT  0x03
#define CST_GEST_SLIDE_RIGHT 0x04
#define CST_GEST_SINGLE_TAP  0x05
#define CST_GEST_DOUBLE_TAP  0x0B
#define CST_GEST_LONG_PRESS  0x0C

// Minimum start→end distance in pixels to call a gesture a swipe when
// the hardware gesture register didn't self-report. 40 px is about
// 11% of the 360 px panel — enough to reject finger jitter during a
// tap, forgiving enough to register a deliberate-but-short swipe.
static constexpr int16_t SWIPE_MIN_PX = 40;

// Internal helper: read the 6-byte touch frame starting at register
// 0x01 (gesture, fingerNum, XH, XL, YH, YL). Returns false on I2C
// failure — caller should treat that as "no event this frame".
static bool read_frame(uint8_t out[6]) {
  Wire.beginTransmission(CST816S_I2C_ADDR);
  Wire.write((uint8_t)0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if ((int)Wire.requestFrom((int)CST816S_I2C_ADDR, 6) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = Wire.read();
  return true;
}

bool touch_init(void) {
  pinMode(TOUCH_PIN_NUM_RST, OUTPUT);
  digitalWrite(TOUCH_PIN_NUM_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_PIN_NUM_RST, HIGH);
  delay(100);

  if (!Wire.begin((int)TOUCH_PIN_NUM_I2C_SDA,
                  (int)TOUCH_PIN_NUM_I2C_SCL,
                  (uint32_t)400000)) {
    return false;
  }
  return true;
}

bool touch_poll_tap(struct TouchTap* out) {
  static bool prevPressed = false;

  uint8_t buf[6];
  if (!read_frame(buf)) return false;

  const uint8_t fingers = buf[1] & 0x0F;
  const bool    pressed = (fingers > 0);

  bool edge = false;
  if (pressed && !prevPressed) {
    edge = true;
    if (out) {
      out->x = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
      out->y = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
    }
  }
  prevPressed = pressed;
  return edge;
}

bool touch_poll_event(enum TouchEvent* evt, struct TouchTap* pos) {
  static bool     prevPressed = false;
  static int16_t  startX      = 0;
  static int16_t  startY      = 0;
  static int16_t  lastX       = 0;
  static int16_t  lastY       = 0;
  // Some CST816S variants latch the gesture code only during the
  // contact window; cache whatever we see while pressed so we can
  // still use it on release even if the register resets to 0.
  static uint8_t  heldGesture = 0;

  uint8_t buf[6];
  if (!read_frame(buf)) return false;

  const uint8_t gesture = buf[0];
  const uint8_t fingers = buf[1] & 0x0F;
  const bool    pressed = (fingers > 0);
  const int16_t x = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
  const int16_t y = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);

  bool report = false;
  enum TouchEvent e = TOUCH_NONE;

  if (pressed) {
    if (!prevPressed) {
      startX = x;
      startY = y;
      heldGesture = 0;
    }
    lastX = x;
    lastY = y;
    if (gesture != 0) heldGesture = gesture;
  } else if (prevPressed) {
    // Release edge. Decide between swipe/tap using the vendor code if
    // it looks like a swipe; otherwise derive from vector length.
    uint8_t g = (gesture != 0) ? gesture : heldGesture;
    switch (g) {
      case CST_GEST_SLIDE_LEFT:  e = TOUCH_SWIPE_LEFT;  break;
      case CST_GEST_SLIDE_RIGHT: e = TOUCH_SWIPE_RIGHT; break;
      case CST_GEST_SLIDE_UP:    e = TOUCH_SWIPE_UP;    break;
      case CST_GEST_SLIDE_DOWN:  e = TOUCH_SWIPE_DOWN;  break;
      default: {
        int16_t dx = (int16_t)(lastX - startX);
        int16_t dy = (int16_t)(lastY - startY);
        int16_t adx = dx < 0 ? (int16_t)-dx : dx;
        int16_t ady = dy < 0 ? (int16_t)-dy : dy;
        if (adx > ady && adx >= SWIPE_MIN_PX) {
          e = (dx > 0) ? TOUCH_SWIPE_RIGHT : TOUCH_SWIPE_LEFT;
        } else if (ady > adx && ady >= SWIPE_MIN_PX) {
          e = (dy > 0) ? TOUCH_SWIPE_DOWN : TOUCH_SWIPE_UP;
        } else {
          e = TOUCH_TAP;
        }
      } break;
    }
    report = true;
    heldGesture = 0;
  }
  prevPressed = pressed;

  if (report) {
    if (evt) *evt = e;
    if (pos) { pos->x = lastX; pos->y = lastY; }
    return true;
  }
  return false;
}
