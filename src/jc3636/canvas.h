#pragma once

#include <stdint.h>
#include <stddef.h>

// Lightweight TFT_eSPI-lookalike that rasterizes into an RGB565
// framebuffer (byte-swapped so the buffer can be shipped directly to
// the ST77916 via esp_lcd_panel_draw_bitmap — the panel expects
// big-endian 16-bit pixels). Only the methods used by the buddy
// renderer are implemented: fillRect / fill / setTextSize /
// setTextColor / setCursor / print, plus drawChar for direct access.
//
// Coords are canvas-local; the caller decides where to blit the buffer
// onto the physical display.
class Canvas {
public:
  Canvas(int w, int h);
  ~Canvas();

  int width()  const { return w_; }
  int height() const { return h_; }
  // Byte-swapped RGB565. Pass straight to display_draw_rect().
  const uint16_t* pixels() const { return buf_; }

  void fill(uint16_t rgb565);
  void fillRect(int x, int y, int w, int h, uint16_t rgb565);

  // Per the TFT_eSPI convention, size is a pixel-multiplier. Only 1 and
  // 2 are used by the buddy code; anything larger works but uses more
  // fills per glyph.
  void setTextSize(uint8_t size)                    { scale_ = size ? size : 1; }
  void setTextColor(uint16_t fg, uint16_t bg)       { fg_ = fg; bg_ = bg; }
  void setCursor(int x, int y)                      { cx_ = x; cy_ = y; }

  // Writes a single character at the cursor and advances it. Handles
  // `\n` (newline: resets x, drops y by one line height) so callers
  // can `print("foo\nbar")` if they want — the buddy code doesn't.
  void print(char c);
  void print(const char* s);

  // Draw a single ASCII glyph at (x, y) without touching the cursor.
  // Used by the centered-sprite helper in buddy_blob.cpp.
  void drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, uint8_t scale);

private:
  void putPixelFast(int x, int y, uint16_t swapped);

  int w_;
  int h_;
  uint16_t* buf_;

  int cx_ = 0, cy_ = 0;
  uint16_t fg_ = 0xFFFF, bg_ = 0x0000;
  uint8_t  scale_ = 1;
};
