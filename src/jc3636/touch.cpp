#include "touch.h"
#include "pincfg.h"
#include <Arduino.h>
#include <Wire.h>

// 7-bit I2C address, confirmed by the esp_lcd_touch_cst816s component's
// default. Some JC3636 revs ship a CST820 instead; its register map is
// a superset so the 6-byte read at 0x01 still works.
#define CST816S_I2C_ADDR 0x15

bool touch_init(void) {
  pinMode(TOUCH_PIN_NUM_RST, OUTPUT);
  digitalWrite(TOUCH_PIN_NUM_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_PIN_NUM_RST, HIGH);
  delay(100);  // CST816S boot window

  // 400 kHz matches what display.c uses in the ESP-IDF reference build.
  if (!Wire.begin((int)TOUCH_PIN_NUM_I2C_SDA,
                  (int)TOUCH_PIN_NUM_I2C_SCL,
                  (uint32_t)400000)) {
    return false;
  }
  return true;
}

bool touch_poll_tap(struct TouchTap* out) {
  static bool prevPressed = false;

  Wire.beginTransmission(CST816S_I2C_ADDR);
  Wire.write((uint8_t)0x01);  // start reg: Gesture, FingerNum, XH, XL, YH, YL
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if ((int)Wire.requestFrom((int)CST816S_I2C_ADDR, 6) != 6) {
    return false;
  }

  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();

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
