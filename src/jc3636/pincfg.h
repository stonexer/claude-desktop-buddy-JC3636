#pragma once

// Pin map for the JC3636W518EN (ESP32-S3 round 360x360).
// Subset of the full map; Step 1 only wires the LCD and the BOOT button.

// ── ST77916 QSPI LCD ──
#define TFT_BLK   15
#define TFT_RST   47
#define TFT_CS    10
#define TFT_SCK    9
#define TFT_SDA0  11
#define TFT_SDA1  12
#define TFT_SDA2  13
#define TFT_SDA3  14

// ── CST816S Touch (I2C) ──
#define TOUCH_PIN_NUM_I2C_SCL   8
#define TOUCH_PIN_NUM_I2C_SDA   7
#define TOUCH_PIN_NUM_INT      41
#define TOUCH_PIN_NUM_RST      40

// ── Button (BOOT) ──
#define BTN_PIN   0
