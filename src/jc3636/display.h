#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH   360
#define DISPLAY_HEIGHT  360

// Initialize LEDC-PWM backlight and ST77916 panel. Safe to call once from
// setup(). On failure, prints via ESP_LOGE and returns false.
bool display_init(void);

// 0..100 percent. 0 powers off backlight; the panel stays initialized.
void display_set_brightness(uint8_t percent);

// Fill the whole screen with a 16-bit RGB565 color. Internally chunked so
// the DMA buffer stays small (~14 KiB).
void display_fill(uint16_t rgb565);

// Draw a rectangular block of RGB565 pixels. `pixels` is row-major with
// `w` pixels per row, `h` rows. The buffer must remain valid until this
// call returns (it does so synchronously).
void display_draw_rect(int x, int y, int w, int h, const uint16_t *pixels);

// Fill a solid-color rectangle. Convenience over display_draw_rect for
// UI blocks (status dots, bars, etc.).
void display_fill_rect(int x, int y, int w, int h, uint16_t rgb565);

#ifdef __cplusplus
}
#endif
