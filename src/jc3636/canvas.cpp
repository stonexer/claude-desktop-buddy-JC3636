#include "canvas.h"
#include "font6x8.h"
#include "esp_heap_caps.h"

#include <string.h>

// Convert host RGB565 to the byte-swapped form stored in the buffer.
// Keeping everything pre-swapped in the backing array avoids a second
// pass before shipping to esp_lcd — every draw call pays for this once,
// so the blit can be a plain memcpy to the panel's DMA path.
static inline uint16_t to_panel(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

Canvas::Canvas(int w, int h) : w_(w), h_(h) {
  const size_t bytes = (size_t)w_ * (size_t)h_ * sizeof(uint16_t);
  buf_ = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (buf_) memset(buf_, 0, bytes);
}

Canvas::~Canvas() {
  if (buf_) heap_caps_free(buf_);
}

void Canvas::fill(uint16_t rgb565) {
  if (!buf_) return;
  const uint16_t v = to_panel(rgb565);
  const size_t n = (size_t)w_ * (size_t)h_;
  for (size_t i = 0; i < n; i++) buf_[i] = v;
}

void Canvas::fillRect(int x, int y, int w, int h, uint16_t rgb565) {
  if (!buf_ || w <= 0 || h <= 0) return;
  if (x >= w_ || y >= h_) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > w_) w = w_ - x;
  if (y + h > h_) h = h_ - y;
  if (w <= 0 || h <= 0) return;

  const uint16_t v = to_panel(rgb565);
  for (int row = 0; row < h; row++) {
    uint16_t* line = buf_ + (size_t)(y + row) * (size_t)w_ + (size_t)x;
    for (int col = 0; col < w; col++) line[col] = v;
  }
}

void Canvas::putPixelFast(int x, int y, uint16_t swapped) {
  if ((unsigned)x >= (unsigned)w_ || (unsigned)y >= (unsigned)h_) return;
  buf_[(size_t)y * (size_t)w_ + (size_t)x] = swapped;
}

void Canvas::drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
  if (!buf_ || scale == 0) return;
  const uint8_t* g = &font[(uint8_t)c * 5];
  const uint16_t fgS = to_panel(fg);
  const uint16_t bgS = to_panel(bg);
  const bool transparent = (fg == bg);  // TFT_eSPI convention: same fg/bg → no erase

  // 6 columns per glyph (5 bitmap + 1 trailing space), 8 rows (7 + 1 spacing)
  for (int col = 0; col < 6; col++) {
    uint8_t bits = (col < 5) ? g[col] : 0x00;
    for (int row = 0; row < 8; row++) {
      bool on = (row < 7) && ((bits >> row) & 1);
      if (!on && transparent) continue;
      uint16_t v = on ? fgS : bgS;
      for (int sy = 0; sy < scale; sy++) {
        const int py = y + row * scale + sy;
        if ((unsigned)py >= (unsigned)h_) continue;
        for (int sx = 0; sx < scale; sx++) {
          const int px = x + col * scale + sx;
          if ((unsigned)px >= (unsigned)w_) continue;
          buf_[(size_t)py * (size_t)w_ + (size_t)px] = v;
        }
      }
    }
  }
}

void Canvas::print(char c) {
  if (c == '\n') {
    cx_ = 0;
    cy_ += 8 * scale_;
    return;
  }
  drawChar(cx_, cy_, c, fg_, bg_, scale_);
  cx_ += 6 * scale_;
}

void Canvas::print(const char* s) {
  if (!s) return;
  while (*s) print(*s++);
}
