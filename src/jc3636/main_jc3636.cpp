// JC3636W518EN entry point for the claude-desktop-buddy port.
//
// Layout on the 360×360 round panel, fit inside the 180-radius
// inscribed circle:
//
//       ┌─────────────────┐
//       │   R:3  W:1  T:5 │   pills at 12 o'clock (240×28 @ y=30)
//       │                 │
//       │   [ pet canvas ]│   220×200 centered
//       │                 │
//       │   3 sessions    │   msg strip at 6 o'clock (280×24 @ y=290)
//       └─────────────────┘
//
// A pending permission prompt overrides the pet canvas with a full-
// bleed PERMISSION screen until the user approves/denies in the
// Desktop app.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "display.h"
#include "canvas.h"
#include "../buddy.h"
#include "ble_bridge.h"
#include "touch.h"
#include "compat/stats.h"

// ───────────────────────── Layout ─────────────────────────────────

// Pet canvas sized to fit a scale-2 blob plus its particle overlays
// (drip y≈134, hearts y≈64). Must stay inside the inscribed circle —
// 220×200 has diagonal ≈ 297 < 360.
static constexpr int CANVAS_W = 220;
static constexpr int CANVAS_H = 200;
static constexpr int CANVAS_X = (DISPLAY_WIDTH  - CANVAS_W) / 2;
static constexpr int CANVAS_Y = (DISPLAY_HEIGHT - CANVAS_H) / 2;

// Top pills: three 66×24 color chips with running/waiting/total.
// Geometry is chosen so the pill corners stay inside the 180 px
// inscribed circle: at y=55, x=70..290, the worst-case corner is
// (70,55), which sits ~166 px from screen center. The label for
// each pill gets the full chip, so trimming width to 66 keeps it
// legible.
static constexpr int TOP_W = 220;
static constexpr int TOP_H = 28;
static constexpr int TOP_X = (DISPLAY_WIDTH - TOP_W) / 2;
static constexpr int TOP_Y = 55;

// Bottom msg strip: 280 wide is near the widest safe chord at
// y=290..314 (distance to center ≈ 137, still < 180).
static constexpr int MSG_W = 280;
static constexpr int MSG_H = 24;
static constexpr int MSG_X = (DISPLAY_WIDTH - MSG_W) / 2;
static constexpr int MSG_Y = 290;

// Status dot at 12 o'clock, above the pills. Small transient badge.
static constexpr int DOT_SIZE = 12;
static constexpr int DOT_X    = (DISPLAY_WIDTH - DOT_SIZE) / 2;
static constexpr int DOT_Y    = 10;
static constexpr uint32_t DOT_HOLD_MS = 10000;

// Color palette — muted so the pet stays the focal point. Pills and
// msg strip live on flat black, text-only, no heavy fills.
static constexpr uint16_t COL_BG      = 0x0000;
static constexpr uint16_t COL_WHITE   = 0xFFFF;
static constexpr uint16_t COL_DIM     = 0x8410;  // medium grey
static constexpr uint16_t COL_RUN     = 0x07FF;  // cyan
static constexpr uint16_t COL_WAIT    = 0xFC00;  // orange
static constexpr uint16_t COL_TOTAL   = 0xBDF7;  // soft grey-white
static constexpr uint16_t COL_ALERT   = 0xF800;  // red (permission)
static constexpr uint16_t COL_PROMPT  = 0xFFE0;  // yellow (prompt tool)

// ───────────────────────── State model ────────────────────────────

enum PersonaState : uint8_t {
  P_SLEEP = 0, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART
};
static const char* STATE_NAMES[7] = {
  "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"
};

struct TamaState {
  uint8_t  sessionsTotal   = 0;
  uint8_t  sessionsRunning = 0;
  uint8_t  sessionsWaiting = 0;
  bool     recentlyCompleted = false;
  bool     connected = false;
  uint32_t lastUpdateMs = 0;
  char     msg[40]        = {0};
  uint32_t tokensToday    = 0;
  // Pending permission prompt (REFERENCE.md "Permission decisions").
  // Non-empty promptId means the Desktop is waiting on a decision.
  char     promptId[40]   = {0};
  char     promptTool[20] = {0};
  char     promptHint[48] = {0};
  // Bumps when the prompt identity changes so the UI knows to redraw.
  uint16_t promptGen = 0;
};

// Owner name loaded from NVS at boot + overwritten when the desktop
// sends {"cmd":"owner","name":"..."}. Empty until we hear from the
// desktop for the first time. msg strip uses it as a "Hi, Name" in
// idle so the device feels personal.
static char g_owner[32] = {0};

// Level progression — persisted. g_level bumps one per 50 000 tokens
// of output (whatever the desktop reports in `tokens_today`). The
// delta calculation lives in applyJson; the progress bar on the Info
// page reads g_tokensIntoLevel to show how close we are to levelling
// up. On boot the level is restored from NVS; progress-within-level
// is not persisted (deliberate — fresh boots start "empty" toward
// the next level, keeping expectations honest).
static constexpr uint32_t TOKENS_PER_LEVEL = 50000;
static uint8_t  g_level             = 0;
static uint32_t g_tokensIntoLevel   = 0;
static uint32_t g_lastTokensToday   = 0;  // for delta computation

// View mode — swipe up switches to the Info page; swipe down returns
// to the pet. The permission overlay still preempts both views.
enum ViewMode : uint8_t {
  VIEW_PET = 0,
  VIEW_INFO,
};
static ViewMode g_view = VIEW_PET;

static TamaState tama;
static PersonaState activeState = P_IDLE;
static uint32_t oneShotUntilMs = 0;
static PersonaState oneShotState = P_IDLE;

static bool demoMode = false;
static uint8_t demoStateIdx = 0;

static constexpr uint32_t LIVE_TIMEOUT_MS = 30000;

static bool dataLive() {
  return tama.lastUpdateMs != 0 && (millis() - tama.lastUpdateMs) <= LIVE_TIMEOUT_MS;
}

static PersonaState derive(const TamaState& s) {
  if (!bleConnected())       return P_IDLE;
  if (!dataLive())           return P_IDLE;
  if (s.sessionsWaiting > 0) return P_ATTENTION;
  if (s.recentlyCompleted)   return P_CELEBRATE;
  if (s.sessionsRunning >= 3) return P_BUSY;
  return P_IDLE;
}

static void triggerOneShot(PersonaState st, uint32_t durMs) {
  oneShotState = st;
  oneShotUntilMs = millis() + durMs;
}

// ───────────────────────── JSON ingest ────────────────────────────

static void sendJsonLine(const char* line) {
  bleWrite((const uint8_t*)line, strlen(line));
  static const uint8_t nl = '\n';
  bleWrite(&nl, 1);
}

static void ackCmd(const char* cmd, bool ok) {
  char out[96];
  snprintf(out, sizeof(out), "{\"ack\":\"%s\",\"ok\":%s,\"n\":0}", cmd, ok ? "true" : "false");
  sendJsonLine(out);
}

// Device → desktop permission decision. REFERENCE.md "Permission
// decisions": "once" approves the pending tool call, "deny" rejects it.
// The id must match tama.promptId exactly.
static void sendPermission(const char* id, const char* decision) {
  char out[128];
  snprintf(out, sizeof(out),
           "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
           id, decision);
  sendJsonLine(out);
}

static void ackStatus() {
  char out[320];
  snprintf(out, sizeof(out),
    "{\"ack\":\"status\",\"ok\":true,\"data\":{"
      "\"name\":\"Claude Buddy JC3636\","
      "\"sec\":%s,"
      "\"bat\":{\"pct\":100,\"mV\":5000,\"mA\":0,\"usb\":true},"
      "\"sys\":{\"up\":%lu,\"heap\":%lu},"
      "\"stats\":{\"appr\":0,\"deny\":0,\"vel\":0,\"nap\":0,\"lvl\":1}"
    "}}",
    bleSecure() ? "true" : "false",
    (unsigned long)(millis() / 1000),
    (unsigned long)ESP.getFreeHeap()
  );
  sendJsonLine(out);
}

static void copyField(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = 0; return; }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

static void applyJson(const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  tama.lastUpdateMs = millis();

  if (!doc["time"].isNull()) return;

  const char* cmd = doc["cmd"] | (const char*)nullptr;
  if (cmd) {
    if      (strcmp(cmd, "status") == 0) { ackStatus(); }
    else if (strcmp(cmd, "name")   == 0) { ackCmd("name",   true); }
    else if (strcmp(cmd, "owner")  == 0) {
      const char* nm = doc["name"] | (const char*)nullptr;
      if (nm && nm[0]) {
        copyField(g_owner, sizeof(g_owner), nm);
        ownerSave(g_owner);
      }
      ackCmd("owner", true);
    }
    else if (strcmp(cmd, "unpair") == 0) { bleClearBonds(); ackCmd("unpair", true); }
    else                                 { ackCmd(cmd,      false); }
    return;
  }

  const char* evt = doc["evt"] | (const char*)nullptr;
  if (evt) {
    if (strcmp(evt, "turn") == 0) triggerOneShot(P_CELEBRATE, 2500);
    return;
  }

  // Heartbeat snapshot.
  tama.sessionsTotal     = doc["total"]   | tama.sessionsTotal;
  tama.sessionsRunning   = doc["running"] | tama.sessionsRunning;
  tama.sessionsWaiting   = doc["waiting"] | tama.sessionsWaiting;
  bool wasCompleted = tama.recentlyCompleted;
  tama.recentlyCompleted = doc["completed"] | false;
  if (!wasCompleted && tama.recentlyCompleted) {
    triggerOneShot(P_CELEBRATE, 3500);
  }
  // Compute a positive delta against the last snapshot. tokens_today
  // resets at local midnight on the desktop, so a drop means "new
  // day" — treat the new value as pure delta instead of going
  // negative.
  uint32_t newTokens = doc["tokens_today"] | tama.tokensToday;
  uint32_t delta = (newTokens >= g_lastTokensToday)
                      ? (newTokens - g_lastTokensToday)
                      : newTokens;
  g_lastTokensToday = newTokens;
  tama.tokensToday  = newTokens;

  // Roll over tokens into levels. A single heartbeat can in principle
  // carry a huge jump (e.g. first hb after a long offline period) so
  // the while-loop handles multi-level increments.
  if (delta > 0) {
    g_tokensIntoLevel += delta;
    while (g_tokensIntoLevel >= TOKENS_PER_LEVEL) {
      g_tokensIntoLevel -= TOKENS_PER_LEVEL;
      if (g_level < 255) g_level++;
      levelSave(g_level);
      triggerOneShot(P_CELEBRATE, 5000);  // longer than turn-end so it reads as "level up"
    }
  }
  const char* m = doc["msg"] | (const char*)nullptr;
  if (m) copyField(tama.msg, sizeof(tama.msg), m);

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]   | (const char*)nullptr;
    const char* pt  = pr["tool"] | (const char*)nullptr;
    const char* ph  = pr["hint"] | (const char*)nullptr;
    bool changed = (pid && strcmp(pid, tama.promptId) != 0);
    copyField(tama.promptId,   sizeof(tama.promptId),   pid);
    copyField(tama.promptTool, sizeof(tama.promptTool), pt);
    copyField(tama.promptHint, sizeof(tama.promptHint), ph);
    if (changed) tama.promptGen++;
  } else if (tama.promptId[0]) {
    tama.promptId[0] = 0;
    tama.promptTool[0] = 0;
    tama.promptHint[0] = 0;
    tama.promptGen++;
  }
}

static char   lineBuf[1024];
static size_t lineLen = 0;

static void drainBLE() {
  while (bleAvailable() > 0) {
    int b = bleRead();
    if (b < 0) break;
    if (b == '\n' || b == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        if (lineBuf[0] == '{') applyJson(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = (char)b;
    } else {
      lineLen = 0;
    }
  }
}

// ───────────────────────── Render ─────────────────────────────────

// `spr` is the pet canvas — exposed as a global so buddy_mux.cpp can
// pick it up via `extern Canvas spr`. The M5 port had the same pattern
// (global TFT_eSprite spr). petCanvas is a convenience alias.
Canvas spr(CANVAS_W, CANVAS_H);
static Canvas* petCanvas  = &spr;
static Canvas* topCanvas  = nullptr;
static Canvas* msgCanvas  = nullptr;
// One-line scratch canvas used by the Info view. Wide enough for
// 300 px text blocks and tall enough (40) to hold a scale-4 glyph
// (32 px). Reused for every row so the Info layout can mix font
// sizes without allocating per-draw.
static Canvas* lineCanvas = nullptr;
static constexpr int LINE_W = 300;
static constexpr int LINE_H = 40;
static constexpr int LINE_X = (DISPLAY_WIDTH - LINE_W) / 2;
static uint32_t tickCount = 0;
static uint32_t nextTickAtMs = 0;
static constexpr uint32_t TICK_MS = 200;

// Center-align `text` horizontally in a canvas of width W at the given
// row y, using fg on bg. `scale` controls font size (1 or 2).
static void drawCentered(Canvas* c, int W, int y, const char* text,
                         uint16_t fg, uint16_t bg, uint8_t scale) {
  int len = (int)strlen(text);
  int w = len * 6 * scale;
  int x = (W - w) / 2;
  if (x < 0) x = 0;
  c->setTextSize(scale);
  c->setTextColor(fg, bg);
  c->setCursor(x, y);
  c->print(text);
}

// One R/W/T "chip": soft-rounded-looking color pill. We fake the
// rounded caps by overlaying two small squares in the bg color at
// each corner — cheap and good enough at this size.
static void drawPill(Canvas* c, int x, int y, int w, int h,
                     uint16_t pillColor, const char* label) {
  c->fillRect(x, y, w, h, pillColor);
  // Corner fake-rounding: 2×2 bg squares at each corner.
  c->fillRect(x,         y,         2, 2, COL_BG);
  c->fillRect(x + w - 2, y,         2, 2, COL_BG);
  c->fillRect(x,         y + h - 2, 2, 2, COL_BG);
  c->fillRect(x + w - 2, y + h - 2, 2, 2, COL_BG);
  // Centered label (size 2, 12×16 per char).
  int len = (int)strlen(label);
  int tw  = len * 12;
  int tx  = x + (w - tw) / 2;
  int ty  = y + (h - 16) / 2;
  c->setTextSize(2);
  c->setTextColor(COL_BG, pillColor);  // punched-out text
  c->setCursor(tx, ty);
  c->print(label);
}

static void paintTop() {
  static uint8_t lastR = 0xFF, lastW = 0xFF, lastT = 0xFF;
  static bool lastDemo = false;
  static int lastDemoIdx = -1;

  // During demo we blank the pills so the screen reads clearly as
  // "demo mode". State name lives in the bottom strip.
  if (demoMode) {
    if (!lastDemo) {
      topCanvas->fill(COL_BG);
      display_draw_rect(TOP_X, TOP_Y, TOP_W, TOP_H, topCanvas->pixels());
      lastDemo = true;
      lastR = lastW = lastT = 0xFF;
    }
    return;
  }
  lastDemo = false;

  if (tama.sessionsRunning == lastR &&
      tama.sessionsWaiting == lastW &&
      tama.sessionsTotal   == lastT) return;
  lastR = tama.sessionsRunning;
  lastW = tama.sessionsWaiting;
  lastT = tama.sessionsTotal;

  topCanvas->fill(COL_BG);

  // Three 66×24 pills, 11 px gap, total 220 — matches TOP_W.
  const int pw = 66, ph = 24, gap = 11;
  const int y0 = (TOP_H - ph) / 2;

  char buf[12];
  snprintf(buf, sizeof(buf), "R %u", (unsigned)tama.sessionsRunning);
  drawPill(topCanvas, 0,                 y0, pw, ph, COL_RUN,   buf);
  snprintf(buf, sizeof(buf), "W %u", (unsigned)tama.sessionsWaiting);
  drawPill(topCanvas, pw + gap,          y0, pw, ph, COL_WAIT,  buf);
  snprintf(buf, sizeof(buf), "T %u", (unsigned)tama.sessionsTotal);
  drawPill(topCanvas, 2*pw + 2*gap,      y0, pw, ph, COL_TOTAL, buf);

  display_draw_rect(TOP_X, TOP_Y, TOP_W, TOP_H, topCanvas->pixels());
}

static void paintMsg() {
  static char lastMsg[48] = "\x01";  // forces first paint
  static bool lastDemo = false;
  static int  lastDemoIdx = -1;

  char target[48];
  if (demoMode) {
    snprintf(target, sizeof(target), "demo: %s", STATE_NAMES[demoStateIdx]);
  } else if (!bleConnected()) {
    strncpy(target, "waiting for Claude...", sizeof(target)); target[sizeof(target)-1]=0;
  } else if (!dataLive()) {
    strncpy(target, "link stale", sizeof(target)); target[sizeof(target)-1]=0;
  } else if (tama.msg[0]) {
    strncpy(target, tama.msg, sizeof(target)); target[sizeof(target)-1]=0;
  } else if (tama.sessionsRunning > 0 || tama.sessionsWaiting > 0 || tama.sessionsTotal > 0) {
    snprintf(target, sizeof(target), "%u session%s",
             (unsigned)tama.sessionsTotal, tama.sessionsTotal == 1 ? "" : "s");
  } else if (g_owner[0]) {
    snprintf(target, sizeof(target), "hi, %s", g_owner);
  } else {
    strncpy(target, "idle", sizeof(target)); target[sizeof(target)-1]=0;
  }

  if (strcmp(target, lastMsg) == 0) return;
  strncpy(lastMsg, target, sizeof(lastMsg)); lastMsg[sizeof(lastMsg)-1]=0;

  msgCanvas->fill(COL_BG);
  // Scale 1 (6×8) keeps the strip quiet — the whole point of the
  // bottom row is to be glanceable, not shouty. 46 chars fit at
  // 280 px wide, plenty for "approve: Bash" / "N sessions" / etc.
  const int scale    = 1;
  const int maxChars = MSG_W / (6 * scale);
  char trimmed[64];
  int srcLen = (int)strlen(target);
  int keep = srcLen < maxChars ? srcLen : maxChars;
  memcpy(trimmed, target, keep);
  trimmed[keep] = 0;

  uint16_t color = (!bleConnected() || !dataLive()) ? COL_DIM : COL_WHITE;
  drawCentered(msgCanvas, MSG_W, (MSG_H - 8 * scale) / 2, trimmed, color, COL_BG, scale);

  display_draw_rect(MSG_X, MSG_Y, MSG_W, MSG_H, msgCanvas->pixels());
}

// Paint one text row onto the shared lineCanvas and blit it to (LINE_X,
// y). Caller supplies scale so headings/body mix cleanly. `rowH` is the
// pixel height of this slot on screen — the text is vertically
// centered within it; the full LINE_W × rowH area is wiped first so
// nothing ghosts between frames.
static void drawInfoRow(int y, int rowH, const char* text, uint16_t fg, uint8_t scale) {
  lineCanvas->fill(COL_BG);
  const int len = (int)strlen(text);
  const int w   = len * 6 * scale;
  const int x   = (LINE_W - w) / 2;
  const int ty  = (rowH - 8 * scale) / 2;
  lineCanvas->setTextSize(scale);
  lineCanvas->setTextColor(fg, COL_BG);
  lineCanvas->setCursor(x < 0 ? 0 : x, ty < 0 ? 0 : ty);
  lineCanvas->print(text);
  display_draw_rect(LINE_X, y, LINE_W, rowH, lineCanvas->pixels());
}

// Info view — dashboard that fills the 360×360 panel between the top
// pills (ends at y=83) and the bottom msg strip (starts at y=290).
// Font sizes escalate for emphasis: LVL is scale-4 (24×32 glyphs) so
// it reads across the room; service/system rows are scale-1.
//
// Layout (y coords are on the physical panel, not canvas-local):
//   95  INFO        (scale 3, white, 24 px tall)
//   130 hi, <owner> (scale 2, 16)
//   170 LVL N       (scale 4, cyan, 32)
//   212 ──progress──(4 px bar)
//   230 tokens N    (scale 2, 16)
//   255 species N   (scale 2, 16)
//   280 up … heap … (scale 1, 8)
static void paintInfoPage() {
  // First, wipe the pet area so no animation residue sits under the
  // text (the pet canvas is 220×200 centered, but we're painting in a
  // 300-wide band — clear a rect that fully covers it).
  display_fill_rect(LINE_X, 85, LINE_W, 205, COL_BG);

  drawInfoRow(100, 12, "INFO", COL_WHITE, 1);

  char buf[48];
  if (g_owner[0]) snprintf(buf, sizeof(buf), "hi, %s", g_owner);
  else            snprintf(buf, sizeof(buf), "(no owner set)");
  drawInfoRow(120, 12, buf, g_owner[0] ? COL_WHITE : COL_DIM, 1);

  snprintf(buf, sizeof(buf), "LVL %u", (unsigned)g_level);
  drawInfoRow(150, 20, buf, COL_RUN, 2);

  // Progress bar, drawn directly onto the panel.
  const int barW = 200;
  const int barX = (DISPLAY_WIDTH - barW) / 2;
  const int barY = 180;
  const int barH = 3;
  int progressPx = (int)((uint64_t)barW * g_tokensIntoLevel / TOKENS_PER_LEVEL);
  if (progressPx > barW) progressPx = barW;
  display_fill_rect(barX, barY, barW, barH, COL_DIM);
  if (progressPx > 0) display_fill_rect(barX, barY, progressPx, barH, COL_RUN);

  snprintf(buf, sizeof(buf), "tokens %lu", (unsigned long)tama.tokensToday);
  drawInfoRow(195, 12, buf, COL_WHITE, 1);

  snprintf(buf, sizeof(buf), "species %s", buddySpeciesName());
  drawInfoRow(215, 12, buf, COL_WHITE, 1);

  uint32_t up = millis() / 1000;
  uint32_t H  = up / 3600;
  uint32_t M  = (up / 60) % 60;
  uint32_t S  = up % 60;
  snprintf(buf, sizeof(buf),
           "up %02lu:%02lu:%02lu  heap %luk",
           (unsigned long)H, (unsigned long)M, (unsigned long)S,
           (unsigned long)(ESP.getFreeHeap() / 1024));
  drawInfoRow(240, 12, buf, COL_DIM, 1);
}

static void paintPetFrame(PersonaState st) {
  // buddyTick handles its own per-frame gating + canvas clear; it
  // writes straight into the global `spr` (== *petCanvas).
  buddyTick((uint8_t)st);
  display_draw_rect(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H, petCanvas->pixels());
}

// Paints the permission-request takeover screen into the pet canvas.
// The pet is hidden while a prompt is pending — the approve/deny
// action happens on the Desktop, we just surface what's being asked
// so the user isn't reaching for the app wondering what's pending.
static void paintPromptOverlay() {
  petCanvas->fill(COL_BG);

  // Heading
  drawCentered(petCanvas, CANVAS_W, 24, "PERMISSION", COL_ALERT, COL_BG, 2);

  // Tool name (the thing trying to run)
  const char* tool = tama.promptTool[0] ? tama.promptTool : "?";
  drawCentered(petCanvas, CANVAS_W, 64, tool, COL_PROMPT, COL_BG, 2);

  // Hint — wrap naively to two lines at ~30 chars each (size 1)
  const char* hint = tama.promptHint[0] ? tama.promptHint : "(no details)";
  const int maxChars = CANVAS_W / 6 - 2;  // 34 chars
  int hlen = (int)strlen(hint);
  if (hlen <= maxChars) {
    drawCentered(petCanvas, CANVAS_W, 110, hint, COL_WHITE, COL_BG, 1);
  } else {
    char line1[48], line2[48];
    // Break at the last space inside maxChars, else hard-cut.
    int brk = maxChars;
    for (int i = maxChars; i > maxChars / 2; i--) {
      if (hint[i] == ' ') { brk = i; break; }
    }
    int n1 = brk;
    memcpy(line1, hint, n1); line1[n1] = 0;
    int n2 = hlen - brk;
    if (n2 > maxChars) n2 = maxChars;
    memcpy(line2, hint + brk + (hint[brk] == ' ' ? 1 : 0), n2);
    line2[n2] = 0;
    drawCentered(petCanvas, CANVAS_W, 104, line1, COL_WHITE, COL_BG, 1);
    drawCentered(petCanvas, CANVAS_W, 116, line2, COL_WHITE, COL_BG, 1);
  }

  // Footer — hardware action hint. Left half of the screen denies,
  // right half approves; the split is on the physical panel, the
  // petCanvas just shows the legend.
  drawCentered(petCanvas, CANVAS_W, 160, "tap < DENY    APPROVE >", COL_DIM, COL_BG, 1);

  display_draw_rect(CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H, petCanvas->pixels());
}

// ───────────────────────── Status dot ─────────────────────────────

static void paintStatusDot() {
  static uint8_t  lastCode   = 0xFF;
  static uint32_t fadeAtMs   = 0;
  static bool     dotVisible = false;

  uint8_t code;
  if (!bleConnected())  code = 0;
  else if (dataLive())  code = 1;
  else                  code = 2;

  uint32_t now = millis();

  if (code != lastCode) {
    lastCode = code;
    uint16_t color;
    switch (code) {
      case 1:  color = 0x07E0; break;
      case 2:  color = 0xFC00; break;
      default: color = 0x0000; break;
    }
    display_fill_rect(DOT_X, DOT_Y, DOT_SIZE, DOT_SIZE, color);
    fadeAtMs   = now + DOT_HOLD_MS;
    dotVisible = (code != 0);
    return;
  }

  if (dotVisible && (int32_t)(now - fadeAtMs) >= 0) {
    display_fill_rect(DOT_X, DOT_Y, DOT_SIZE, DOT_SIZE, 0x0000);
    dotVisible = false;
  }
}

// ───────────────────────── Button ─────────────────────────────────

static void cycleSpecies(int delta);  // defined below, used by pollButton

// BOOT (GPIO0) semantics:
//   short press  → cycle pet species (persisted to NVS); in demo mode
//                  it cycles the demo state instead so the 7 animations
//                  are still reachable.
//   long press   → toggle demo mode on/off.
static void pollButton() {
  static uint32_t pressStart = 0;
  static bool     wasDown    = false;
  static uint32_t lastEdgeMs = 0;

  bool down = (digitalRead(0) == LOW);
  uint32_t now = millis();
  if (now - lastEdgeMs < 20) return;

  if (down && !wasDown) {
    pressStart = now;
    wasDown = true;
    lastEdgeMs = now;
  } else if (!down && wasDown) {
    uint32_t held = now - pressStart;
    wasDown = false;
    lastEdgeMs = now;
    if (held >= 1000) {
      demoMode = !demoMode;
      demoStateIdx = 0;
    } else if (demoMode) {
      demoStateIdx = (demoStateIdx + 1) % 7;
    } else {
      cycleSpecies(+1);
    }
  }
}

// ───────────────────────── Arduino lifecycle ──────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[boot] claude-buddy JC3636");

  pinMode(0, INPUT_PULLUP);

  if (!display_init()) {
    Serial.println("[boot] display_init FAILED — halting");
    while (true) { delay(1000); }
  }
  display_fill(COL_BG);

  if (!touch_init()) {
    Serial.println("[boot] touch_init failed — approve/deny from device disabled");
    // Non-fatal: the pet and BLE bridge still work, user just can't
    // decide permissions from the hardware.
  }

  // petCanvas already points at the global `spr`. Remaining canvases
  // are heap-allocated — the pet canvas is the largest (220×200), the
  // other two are small strips we can afford in internal SRAM.
  topCanvas  = new Canvas(TOP_W,  TOP_H);
  msgCanvas  = new Canvas(MSG_W,  MSG_H);
  lineCanvas = new Canvas(LINE_W, LINE_H);
  buddyInit();
  ownerLoad(g_owner, sizeof(g_owner));
  g_level = levelLoad();

  bleInit("Claude Buddy JC3636");

  nextTickAtMs = millis();
}

// Wrap the species index in either direction, persist to NVS, and
// trigger a redraw. Shared by BOOT short-press and the left/right
// swipe handlers so they stay in lockstep.
static void cycleSpecies(int delta) {
  uint8_t n = buddySpeciesCount();
  if (n == 0) return;
  int cur = (int)buddySpeciesIdx();
  int next = ((cur + delta) % (int)n + (int)n) % (int)n;
  buddySetSpeciesIdx((uint8_t)next);
  speciesIdxSave((uint8_t)next);
  buddyInvalidate();
}

// Consume whatever touch gesture just fired. Priority order:
//   1. Pending permission prompt + tap → approve (right half) / deny.
//   2. Left/right swipe → cycle species (in demo mode we cycle the
//      demo state instead, so all 7 animations stay reachable).
//   3. Everything else (up/down swipe, stray tap without a prompt)
//      is swallowed for now; future: double-tap → celebrate, etc.
static void handleTouch() {
  TouchEvent e;
  TouchTap   pos;
  if (!touch_poll_event(&e, &pos)) return;

  if (e == TOUCH_TAP && tama.promptId[0] != 0) {
    const char* decision = (pos.x >= DISPLAY_WIDTH / 2) ? "once" : "deny";
    sendPermission(tama.promptId, decision);
    tama.promptId[0]   = 0;
    tama.promptTool[0] = 0;
    tama.promptHint[0] = 0;
    tama.promptGen++;
    return;
  }

  if (e == TOUCH_SWIPE_LEFT || e == TOUCH_SWIPE_RIGHT) {
    const int delta = (e == TOUCH_SWIPE_RIGHT) ? +1 : -1;
    if (demoMode) {
      demoStateIdx = (uint8_t)((demoStateIdx + 7 + delta) % 7);
    } else {
      cycleSpecies(delta);
    }
    return;
  }

  // Up/down swipes page between pet and info. The top/bottom strips
  // (pills, msg) stay regardless of view so BLE state is still visible.
  if (e == TOUCH_SWIPE_UP && g_view == VIEW_PET) {
    g_view = VIEW_INFO;
    buddyInvalidate();  // force pet redraw on return
  } else if (e == TOUCH_SWIPE_DOWN && g_view == VIEW_INFO) {
    g_view = VIEW_PET;
    buddyInvalidate();
  }
}

void loop() {
  drainBLE();
  pollButton();
  handleTouch();

  uint32_t now = millis();
  if ((int32_t)(now - nextTickAtMs) >= 0) {
    nextTickAtMs = now + TICK_MS;
    tickCount++;

    // Prompt takeover wins over everything except demo mode. Demo is
    // explicit user intent and shouldn't get stomped.
    const bool hasPrompt = tama.promptId[0] != 0;

    if (!demoMode && hasPrompt) {
      static uint16_t lastPromptGen = 0xFFFF;
      if (tama.promptGen != lastPromptGen) {
        paintPromptOverlay();
        lastPromptGen = tama.promptGen;
      }
    } else if (g_view == VIEW_INFO) {
      paintInfoPage();
    } else {
      PersonaState desired;
      if (demoMode) {
        desired = (PersonaState)demoStateIdx;
      } else if (now < oneShotUntilMs) {
        desired = oneShotState;
      } else {
        desired = derive(tama);
      }
      activeState = desired;
      paintPetFrame(activeState);
    }

    paintTop();
    paintMsg();
    paintStatusDot();
  }

  delay(10);
}
