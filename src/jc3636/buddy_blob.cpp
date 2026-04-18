// Blob buddy — 7 animated states, ported from the original
// src/buddies/blob.cpp. The only changes vs. the M5 version:
//   - TFT_eSPI / TFT_eSprite / M5.Lcd references replaced with Canvas
//   - buddyPrintLine / buddyPrintSprite rewritten on top of
//     Canvas::drawChar, centering on BUDDY_X_CENTER within the canvas
//
// Geometry is rescaled for the 360x360 round panel:
//   BUDDY_X_CENTER = canvas.width() / 2 — the blob sits in the middle
//   BUDDY_Y_BASE   = 10                 — near the top of the canvas
// Scale is fixed at 2 so the ASCII art is ~160 px tall, big enough to
// read from across the room but still inside the ~254 px inscribed
// square of the circular screen.

#include "buddy_blob.h"
#include "canvas.h"
#include <stdint.h>
#include <string.h>

// Mirrors PersonaState in main_jc3636.cpp
enum { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DIZZY, B_HEART };

// Colors — matched to the original blob palette (RGB565).
static const uint16_t BUDDY_BG     = 0x0000;
static const uint16_t BUDDY_HEART  = 0xF810;
static const uint16_t BUDDY_DIM    = 0x8410;
static const uint16_t BUDDY_YEL    = 0xFFE0;
static const uint16_t BUDDY_WHITE  = 0xFFFF;
static const uint16_t BUDDY_CYAN   = 0x07FF;
static const uint16_t BUDDY_GREEN  = 0x07E0;
static const uint16_t BUDDY_RED    = 0xF800;
static const uint16_t BUDDY_SLIME  = 0x07F0;  // original blob body color
// BUDDY_BLUE / BUDDY_PURPLE exist in the original palette but aren't
// referenced by blob's state functions, so omitted here.

// Canvas-local geometry — BUDDY_Y_BASE and BUDDY_Y_OVERLAY match the
// original M5 layout (30 / 6). They're 1x coordinates; SCALE expands
// them at render time. Keeping the original offset means Y_OVERLAY is
// above the sprite, so sleep-Z / attention-! / heart particles drift
// around the head instead of overlapping the body.
static int BUDDY_X_CENTER = 110;  // patched at init from canvas.width()/2
static const int BUDDY_Y_BASE    = 30;
static const int BUDDY_Y_OVERLAY = 6;
static const int CHAR_W = 6;
static const int CHAR_H = 8;
static const int SCALE  = 2;      // pixels per glyph cell side

// Centered line print. `xOff` shifts the line in character units * SCALE
// pixels (same contract as the original buddyPrintLine).
static void blobPrintLine(Canvas& c, const char* line, int yPx, uint16_t color, int xOff = 0) {
  int len = (int)strlen(line);
  // Trim padding at SCALE>1 so trailing/leading spaces don't push ink off the edge.
  while (len && line[len - 1] == ' ') len--;
  while (len && *line == ' ')         { line++; len--; }
  int w = len * CHAR_W * SCALE;
  int x = BUDDY_X_CENTER - w / 2 + xOff * SCALE;
  for (int i = 0; i < len; i++) {
    c.drawChar(x + i * CHAR_W * SCALE, yPx, line[i], color, BUDDY_BG, SCALE);
  }
}

// 5-line sprite block. yOffset is added to BUDDY_Y_BASE for the top
// row (in 1x coords); same contract as the original buddyPrintSprite.
static void blobPrintSprite(Canvas& c, const char* const* lines, uint8_t nLines,
                            int yOffset, uint16_t color, int xOff = 0) {
  int yBase = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
  for (uint8_t i = 0; i < nLines; i++) {
    blobPrintLine(c, lines[i], yBase + (yOffset + i * CHAR_H) * SCALE, color, xOff);
  }
}

// Ad-hoc single-glyph helpers — match the original particle placement.
// Coords passed in are 1x canvas coords; we scale them here.
static void blobAtChar(Canvas& c, int x, int y, char ch, uint16_t color) {
  int px = BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * SCALE;
  int py = y * SCALE;
  c.drawChar(px, py, ch, color, BUDDY_BG, SCALE);
}

// ─── SLEEP ───  ~12s cycle, 6 poses
static void doSleep(Canvas& c, uint32_t t) {
  static const char* const PUDDLE[5]  = { "            ", "            ", "   .----.   ", "  ( -- -- ) ", "  `~------~`" };
  static const char* const BREATH[5]  = { "            ", "   .----.   ", "  ( -- -- ) ", "  (        )", "   `------` " };
  static const char* const DEEP[5]    = { "            ", "  .------.  ", " ( -- -- ) ", " (         )", "  `~------~`" };
  static const char* const DRIP[5]    = { "            ", "            ", "   .----.   ", "  ( -- -- ) ", "  `--.----` " };
  static const char* const MELT[5]    = { "            ", "            ", "   .----.   ", "  ( __ __ ) ", " `~~~~~~~~~`" };
  static const char* const SNORE[5]   = { "            ", "  .------.  ", " ( __ __ )  ", " (    o    )", "  `~------~`" };

  const char* const* P[6] = { PUDDLE, BREATH, DEEP, DRIP, MELT, SNORE };
  static const uint8_t SEQ[] = {
    0,1,2,1,0,1,2,1,
    0,4,4,0,
    1,2,5,2,1,
    3,3,0,0,
    1,2,1,0
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  blobPrintSprite(c, P[SEQ[beat]], 5, 0, BUDDY_SLIME);

  int p1 = (t)     % 10;
  int p2 = (t + 4) % 10;
  int p3 = (t + 7) % 10;
  blobAtChar(c, BUDDY_X_CENTER + 20 + p1, BUDDY_Y_OVERLAY + 18 - p1 * 2, 'z', BUDDY_DIM);
  blobAtChar(c, BUDDY_X_CENTER + 26 + p2, BUDDY_Y_OVERLAY + 14 - p2,     'Z', BUDDY_WHITE);
  blobAtChar(c, BUDDY_X_CENTER + 16 + p3 / 2, BUDDY_Y_OVERLAY + 10 - p3 / 2, 'z', BUDDY_DIM);

  int dphase = (t / 2) % 12;
  blobAtChar(c, BUDDY_X_CENTER - 6, BUDDY_Y_BASE + 26 + dphase, dphase < 8 ? '.' : ' ', BUDDY_SLIME);
}

// ─── IDLE ───  ~14s cycle, 10 poses
static void doIdle(Canvas& c, uint32_t t) {
  static const char* const SMALL[5]   = { "            ", "    .--.    ", "   (o  o)   ", "   (    )   ", "    `--`    " };
  static const char* const MED[5]     = { "            ", "   .----.   ", "  ( o  o )  ", "  (      )  ", "   `----`   " };
  static const char* const BIG[5]     = { "            ", "  .------.  ", " ( o    o ) ", " (        ) ", "  `------`  " };
  static const char* const LOOK_L[5]  = { "            ", "   .----.   ", "  (o   o )  ", "  (      )  ", "   `----`   " };
  static const char* const LOOK_R[5]  = { "            ", "   .----.   ", "  ( o   o)  ", "  (      )  ", "   `----`   " };
  static const char* const BLINK[5]   = { "            ", "   .----.   ", "  ( -  - )  ", "  (      )  ", "   `----`   " };
  static const char* const WIGGLE_L[5]= { "            ", "  .----.    ", " ( o  o )   ", " (      )   ", "  `----`    " };
  static const char* const WIGGLE_R[5]= { "            ", "    .----.  ", "   ( o  o ) ", "   (      ) ", "    `----`  " };
  static const char* const JIGGLE[5]  = { "            ", "  .~~~~~~.  ", " ( o    o ) ", " (        ) ", "  `~~~~~~`  " };
  static const char* const DRIP_S[5]  = { "            ", "   .----.   ", "  ( o  o )  ", "  (      )  ", "  `--.--.`  " };

  const char* const* P[10] = { SMALL, MED, BIG, LOOK_L, LOOK_R, BLINK, WIGGLE_L, WIGGLE_R, JIGGLE, DRIP_S };
  static const uint8_t SEQ[] = {
    1,2,1,0,1,2, 1,3,1,4,1,5,
    2,2,8,8,2,
    6,7,6,7, 1,1,
    2,9,9,1,
    1,3,4,3,4,1,5,1,
    0,0,2,2
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  blobPrintSprite(c, P[SEQ[beat]], 5, 0, BUDDY_SLIME);
}

// ─── BUSY ───  ~10s cycle, 6 poses + dot ticker
static void doBusy(Canvas& c, uint32_t t) {
  static const char* const FOCUS_A[5] = { "            ", "   .----.   ", "  ( v  v )  ", "  (   --  ) ", "   `----`   " };
  static const char* const FOCUS_B[5] = { "            ", "   .----.   ", "  ( v  v )  ", "  (   __  ) ", "   `----`   " };
  static const char* const CHURN[5]   = { "            ", "  .~----~.  ", " ( v    v ) ", " (   oo   )", "  `~----~`  " };
  static const char* const THINK[5]   = { "      ?     ", "   .----.   ", "  ( ^  ^ )  ", "  (   ..  ) ", "   `----`   " };
  static const char* const PROCESS[5] = { "      *     ", "  .------.  ", " ( O    O ) ", " (   ==   )", "  `------`  " };
  static const char* const DRIP_W[5]  = { "            ", "   .----.   ", "  ( v  v )  ", "  (   --  ) ", "  `--.----` " };

  const char* const* P[6] = { FOCUS_A, FOCUS_B, CHURN, THINK, PROCESS, DRIP_W };
  static const uint8_t SEQ[] = {
    0,1,0,1,0,1, 2,2, 0,1,0,1, 3,3, 4,4, 0,1,5,5,2
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  blobPrintSprite(c, P[SEQ[beat]], 5, 0, BUDDY_SLIME);

  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  int dotsIdx = t % 6;
  // Render the 3-char dots ticker to the right of the body. We reuse the
  // per-char helper so it picks up the SCALE transform.
  int dx0 = BUDDY_X_CENTER + 22;
  int dy  = BUDDY_Y_OVERLAY + 14;
  for (int i = 0; i < 3; i++) {
    blobAtChar(c, dx0 + i * CHAR_W, dy, DOTS[dotsIdx][i], BUDDY_WHITE);
  }

  int b = (t / 2) % 8;
  blobAtChar(c, BUDDY_X_CENTER - 2, BUDDY_Y_OVERLAY + 18 - b, b < 6 ? 'o' : ' ', BUDDY_CYAN);
}

// ─── ATTENTION ───  ~8s cycle, 6 poses + ! pulse
static void doAttention(Canvas& c, uint32_t t) {
  static const char* const TALL[5]    = { "    .--.    ", "   (    )   ", "  ( O  O )  ", "  (   !   ) ", "  `------`  " };
  static const char* const PEEK_L[5]  = { "    .--.    ", "   (    )   ", " ( O  O  )  ", " (   !    ) ", " `------`   " };
  static const char* const PEEK_R[5]  = { "    .--.    ", "   (    )   ", "  ( O  O )  ", "   (   !  ) ", "   `------` " };
  static const char* const STRETCH[5] = { "     ||     ", "    /  \\    ", "  ( O  O )  ", "  (   !   ) ", "  `------`  " };
  static const char* const TENSE[5]   = { "    .--.    ", "  /(    )\\  ", " /( O  O )\\ ", " (   !!   ) ", " /`------`\\ " };
  static const char* const SHRINK[5]  = { "            ", "    .--.    ", "   (O  O)   ", "   (  !  )  ", "    `--`    " };

  const char* const* P[7] = { TALL, PEEK_L, TALL, PEEK_R, STRETCH, TENSE, SHRINK };
  static const uint8_t SEQ[] = {
    0,4,0,1,0,2,0,3, 4,4,0,1,2,0, 5,3, 0,0,6,0
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  uint8_t pose = SEQ[beat];
  int xOff = (pose == 4) ? ((t & 1) ? 1 : -1) : 0;
  blobPrintSprite(c, P[pose], 5, 0, BUDDY_SLIME, xOff);

  if ((t / 2) & 1) blobAtChar(c, BUDDY_X_CENTER - 8, BUDDY_Y_OVERLAY - 4, '!', BUDDY_YEL);
  if ((t / 3) & 1) blobAtChar(c, BUDDY_X_CENTER + 8, BUDDY_Y_OVERLAY,     '!', BUDDY_RED);
  if ((t / 4) & 1) blobAtChar(c, BUDDY_X_CENTER,     BUDDY_Y_OVERLAY - 8, '!', BUDDY_YEL);
}

// ─── CELEBRATE ───  ~5.6s cycle, 6 poses + confetti rain
static void doCelebrate(Canvas& c, uint32_t t) {
  static const char* const SQUASH[5]  = { "            ", "            ", "  .--------.", " ( ^      ^)", " `~~------~`" };
  static const char* const LAUNCH[5]  = { "            ", "   .----.   ", "  ( ^  ^ )  ", " /(  ww  )\\ ", "  `------`  " };
  static const char* const AIRBORNE[5]= { "    .--.    ", "   ( ^^ )   ", "   (  WW)   ", "    `--`    ", "    : :     " };
  static const char* const SPLAT_L[5] = { "            ", "            ", " .---------.", "( ^      ^ )", " `~~~------`" };
  static const char* const SPLAT_R[5] = { "            ", "            ", ".---------. ", "( ^      ^ )", "`------~~~` " };
  static const char* const POSE[5]    = { "    \\__/    ", "   .----.   ", "  ( *  * )  ", " /(  WW  )\\ ", "  `------`  " };

  const char* const* P[6] = { SQUASH, LAUNCH, AIRBORNE, SPLAT_L, SPLAT_R, POSE };
  static const uint8_t SEQ[] = { 0,1,2,1,0, 3,4,3,4, 0,1,2,1,0, 5,5 };
  static const int8_t Y_SHIFT[] = { 0,-2,-7,-2,0, 0,0,0,0, 0,-2,-7,-2,0, 0,0 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  blobPrintSprite(c, P[SEQ[beat]], 5, Y_SHIFT[beat], BUDDY_SLIME);

  static const uint16_t cols[] = { BUDDY_YEL, BUDDY_HEART, BUDDY_CYAN, BUDDY_SLIME, BUDDY_GREEN };
  for (int i = 0; i < 6; i++) {
    int phase = (t * 2 + i * 11) % 22;
    int x = BUDDY_X_CENTER - 36 + i * 14;
    int y = BUDDY_Y_OVERLAY - 6 + phase;
    if (y > BUDDY_Y_BASE + 20 || y < 0) continue;
    char glyph = ((i + (int)(t/2)) & 1) ? '*' : 'o';
    blobAtChar(c, x, y, glyph, cols[i % 5]);
  }
}

// ─── DIZZY ───  ~5.6s cycle, 5 poses + orbiting stars
static void doDizzy(Canvas& c, uint32_t t) {
  static const char* const LEAN_L[5]  = { "            ", "  .----.    ", " ( @  @ )   ", " (  ~~  )   ", "  `----`    " };
  static const char* const LEAN_R[5]  = { "            ", "    .----.  ", "   ( @  @ ) ", "   (  ~~  ) ", "    `----`  " };
  static const char* const WOBBLE[5]  = { "            ", "  .~----~.  ", " ( x    @ ) ", " (   vv   )", "  `~----~`  " };
  static const char* const WOBBLE2[5] = { "            ", "  .~----~.  ", " ( @    x ) ", " (   vv   )", "  `~----~`  " };
  static const char* const SPLAT[5]   = { "            ", "            ", " .---------.", "( @      @ )", " `--._.--._`" };

  const char* const* P[5] = { LEAN_L, LEAN_R, WOBBLE, WOBBLE2, SPLAT };
  static const uint8_t SEQ[] = { 0,1,0,1, 2,3, 0,1,0,1, 4,4, 2,3 };
  static const int8_t X_SHIFT[] = { -3,3,-3,3, 0,0, -3,3,-3,3, 0,0, 0,0 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  blobPrintSprite(c, P[SEQ[beat]], 5, 0, BUDDY_SLIME, X_SHIFT[beat]);

  static const int8_t OX[] = { 0, 5, 7, 5, 0, -5, -7, -5 };
  static const int8_t OY[] = { -5, -3, 0, 3, 5, 3, 0, -3 };
  uint8_t p1 = t % 8;
  uint8_t p2 = (t + 4) % 8;
  uint8_t p3 = (t + 2) % 8;
  blobAtChar(c, BUDDY_X_CENTER + OX[p1] - 2, BUDDY_Y_OVERLAY + 6 + OY[p1], '*', BUDDY_CYAN);
  blobAtChar(c, BUDDY_X_CENTER + OX[p2] - 2, BUDDY_Y_OVERLAY + 6 + OY[p2], '*', BUDDY_YEL);
  blobAtChar(c, BUDDY_X_CENTER + OX[p3] - 2, BUDDY_Y_OVERLAY + 6 + OY[p3], 'o', BUDDY_WHITE);
}

// ─── HEART ───  ~10s cycle, 5 poses + rising heart stream
static void doHeart(Canvas& c, uint32_t t) {
  static const char* const DREAMY[5]  = { "            ", "   .----.   ", "  ( ^  ^ )  ", "  (   ww  ) ", "   `----`   " };
  static const char* const BLUSH[5]   = { "            ", "   .----.   ", "  (#^  ^#)  ", "  (   ww  ) ", "   `----`   " };
  static const char* const HEART_E[5] = { "            ", "  .------.  ", " ( <3  <3 ) ", " (    v   ) ", "  `------`  " };
  static const char* const MELT_H[5]  = { "            ", "  .~~~~~~.  ", " ( @    @ ) ", " (   ww   )", "  `~------`" };
  static const char* const SIGH[5]    = { "            ", "   .----.   ", "  ( -  - )  ", "  (   ^^  ) ", "   `----`   " };

  const char* const* P[5] = { DREAMY, BLUSH, HEART_E, MELT_H, SIGH };
  static const uint8_t SEQ[] = {
    0,0,1,0, 2,2,0, 1,0,4, 0,0,3,3, 0,1,0,2, 1,0
  };
  static const int8_t Y_BOB[] = { 0,-1,0,-1, 0,-1,0, -1,0,0, -1,0,0,0, -1,0,-1,0, -1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  blobPrintSprite(c, P[SEQ[beat]], 5, Y_BOB[beat], BUDDY_SLIME);

  for (int i = 0; i < 5; i++) {
    int phase = (t + i * 4) % 16;
    int y = BUDDY_Y_OVERLAY + 16 - phase;
    if (y < -2 || y > BUDDY_Y_BASE) continue;
    int x = BUDDY_X_CENTER - 20 + i * 8 + ((phase / 3) & 1) * 2 - 1;
    blobAtChar(c, x, y, 'v', BUDDY_HEART);
  }
}

// ─── dispatcher ───

typedef void (*StateFn)(Canvas&, uint32_t);
static const StateFn STATES[7] = {
  doSleep, doIdle, doBusy, doAttention, doCelebrate, doDizzy, doHeart,
};

void blobInit(Canvas& c) {
  BUDDY_X_CENTER = c.width() / 2;
}

void blobRender(Canvas& c, uint32_t tick, uint8_t state) {
  if (state >= 7) state = B_IDLE;
  c.fill(BUDDY_BG);
  STATES[state](c, tick);
}
