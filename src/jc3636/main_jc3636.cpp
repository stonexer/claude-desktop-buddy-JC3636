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
#include "buddy_blob.h"
#include "ble_bridge.h"
#include "touch.h"

// ───────────────────────── Layout ─────────────────────────────────

// Pet canvas sized to fit a scale-2 blob plus its particle overlays
// (drip y≈134, hearts y≈64). Must stay inside the inscribed circle —
// 220×200 has diagonal ≈ 297 < 360.
static constexpr int CANVAS_W = 220;
static constexpr int CANVAS_H = 200;
static constexpr int CANVAS_X = (DISPLAY_WIDTH  - CANVAS_W) / 2;
static constexpr int CANVAS_Y = (DISPLAY_HEIGHT - CANVAS_H) / 2;

// Top pills: three 70×24 color chips with running/waiting/total.
// 240 total width (3×70 + 2×15 gap) centered at x=60..300. y=30
// keeps the corners of the strip at radius < 175 from center.
static constexpr int TOP_W = 240;
static constexpr int TOP_H = 28;
static constexpr int TOP_X = (DISPLAY_WIDTH - TOP_W) / 2;
static constexpr int TOP_Y = 30;

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
    else if (strcmp(cmd, "owner")  == 0) { ackCmd("owner",  true); }
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
  tama.tokensToday = doc["tokens_today"] | tama.tokensToday;
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

static Canvas* petCanvas = nullptr;
static Canvas* topCanvas = nullptr;
static Canvas* msgCanvas = nullptr;
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

  // Three 70×24 pills, 15 px gap, total 240.
  const int pw = 70, ph = 24, gap = 15;
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
  } else {
    strncpy(target, "idle", sizeof(target)); target[sizeof(target)-1]=0;
  }

  if (strcmp(target, lastMsg) == 0) return;
  strncpy(lastMsg, target, sizeof(lastMsg)); lastMsg[sizeof(lastMsg)-1]=0;

  msgCanvas->fill(COL_BG);
  // Size-1 text (6×8) — truncate to fit the 280 px strip: 46 chars max.
  const int maxChars = MSG_W / 6;
  char trimmed[64];
  int srcLen = (int)strlen(target);
  int keep = srcLen < maxChars ? srcLen : maxChars;
  memcpy(trimmed, target, keep);
  trimmed[keep] = 0;

  uint16_t color = (!bleConnected() || !dataLive()) ? COL_DIM : COL_WHITE;
  drawCentered(msgCanvas, MSG_W, (MSG_H - 8) / 2, trimmed, color, COL_BG, 1);

  display_draw_rect(MSG_X, MSG_Y, MSG_W, MSG_H, msgCanvas->pixels());
}

static void paintPetFrame(PersonaState st) {
  blobRender(*petCanvas, tickCount, (uint8_t)st);
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
      demoMode = false;
    } else {
      demoMode = true;
      demoStateIdx = (demoStateIdx + 1) % 7;
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

  petCanvas = new Canvas(CANVAS_W, CANVAS_H);
  topCanvas = new Canvas(TOP_W,    TOP_H);
  msgCanvas = new Canvas(MSG_W,    MSG_H);
  blobInit(*petCanvas);

  bleInit("Claude Buddy JC3636");

  nextTickAtMs = millis();
}

// Handle a single-tap edge from the CST816S. During a pending permission
// prompt the tap position decides approve vs deny; otherwise we ignore
// it (future: demo-mode toggle, state shortcuts).
static void handleTouch() {
  TouchTap t;
  if (!touch_poll_tap(&t)) return;

  if (tama.promptId[0] != 0) {
    const char* decision = (t.x >= DISPLAY_WIDTH / 2) ? "once" : "deny";
    sendPermission(tama.promptId, decision);
    // Clear locally so we don't double-fire if the desktop's next
    // heartbeat still carries the same prompt. The real server-side
    // clear happens when the desktop processes the decision and sends
    // the next hb without the `prompt` field.
    tama.promptId[0]   = 0;
    tama.promptTool[0] = 0;
    tama.promptHint[0] = 0;
    tama.promptGen++;
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
