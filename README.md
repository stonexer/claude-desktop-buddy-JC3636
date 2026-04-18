# claude-desktop-buddy-jc3636

A port of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
for the **JC3636W518EN** — an ESP32-S3 dev board with a 360×360 round
ST77916 QSPI LCD and a CST816S touch panel.

The original firmware targets the M5StickC Plus (135×240 portrait LCD,
physical buttons, IMU). This port keeps the same wire protocol
(Nordic UART Service, line-delimited JSON — see
[REFERENCE.md](REFERENCE.md)) and the same ASCII pet aesthetic, but
rewrites the platform layer for the round panel and ESP-IDF 5.x via
Arduino-ESP32 3.x.

> **Not affiliated with Anthropic.** This is an independent community
> port. The BLE bridge it talks to ships with the official Claude
> macOS/Windows apps behind a developer-mode flag — see the original
> repo for setup.

## Hardware

| Spec       | Value                                         |
| ---------- | --------------------------------------------- |
| MCU        | ESP32-S3 (16 MB QIO flash, 8 MB OPI PSRAM)    |
| Display    | ST77916 QSPI, 360×360, 16-bit RGB565          |
| Touch      | CST816S over I²C *(not wired up yet)*         |
| Input      | BOOT button (GPIO 0) — demo mode cycling      |
| BLE        | NimBLE host, Nordic UART Service (NUS)        |

Pin map is in `src/jc3636/pincfg.h`.

## Features

- Full 7-state ASCII buddy animation ported from the original (blob
  species — `src/jc3636/buddy_blob.cpp`). Sleep / idle / busy /
  attention / celebrate / dizzy / heart.
- Round-panel UI:
  - Top strip: three pills showing `running / waiting / total`
    session counters (`R / W / T`).
  - Center: pet canvas (220×200).
  - Bottom strip: one-line status message from the heartbeat
    (`approve: Bash`, `3 sessions`, `waiting for Claude…`).
  - Transient status dot at 12 o'clock — flashes on link state
    changes, auto-hides after 10 s so it doesn't clutter the bezel.
- Permission-prompt takeover screen: when the Desktop asks for
  permission to run a tool, the pet steps aside and the screen
  shows the tool name + hint. Approve/deny still happens in the
  Desktop app.
- Demo mode: tap BOOT to cycle through all 7 animations without
  needing a live Claude session. Long-press BOOT (≥1 s) to exit.

## Build

```bash
# Once
pio project init --ide vscode   # optional, for editor support

# Build + flash + monitor
pio run -e jc3636w518en -t upload
pio device monitor -e jc3636w518en
```

The board appears as `/dev/cu.usbmodem1101` on macOS (ESP32-S3 built-in
USB JTAG/Serial). If the first upload hangs mid-flash, hold BOOT while
re-plugging the USB cable to force ROM download mode, then re-run the
upload.

## Pairing

Same as the upstream project:

1. **Claude for macOS/Windows → Help → Troubleshooting → Enable
   Developer Mode**.
2. **Developer → Open Hardware Buddy…** → Connect.
3. Pick `Claude Buddy JC3636` from the scan list.

The current build uses **unencrypted** characteristics + JustWorks
pairing. See *Known limitations* below.

## Triggering pet states

The pet reacts to heartbeat snapshots the Claude Desktop app pushes
over BLE:

| State       | Trigger                                  |
| ----------- | ---------------------------------------- |
| `idle`      | connected, nothing urgent                |
| `busy`      | `running >= 3`                           |
| `attention` | `waiting >= 1` (pending permission)      |
| `celebrate` | `evt:turn` or `completed: true`          |
| `sleep`     | reached only via demo mode in this port  |
| `dizzy`     | demo only (no IMU on this board)         |
| `heart`     | demo only                                |

**Important caveat.** The heartbeat only reflects Claude activity
running *inside the Desktop app process* (the in-app Claude Code
panel). Sessions launched from the standalone `claude` CLI in a
terminal don't show up — see *Known limitations*.

## Known limitations

- **CLI sessions don't feed the heartbeat.** There's no documented
  IPC between the `claude` terminal CLI and the Desktop app's
  Hardware Buddy bridge. If you want the pet to react to CLI work,
  you'd need a separate bridge process (e.g. a Node/Python helper
  that reads `claude --output-format stream-json` and writes NUS
  frames to the device directly). Open to PRs.
- **BLE encryption.** The original M5 firmware runs LE Secure
  Connections with a 6-digit passkey (MITM-protected). Under
  pioarduino's NimBLE stack, JustWorks pairing was the only mode
  that kept the TX CCCD subscribe from silently failing, so this
  port drops MITM for now. Transcript snippets over the link are
  sniffable. Don't pair near an adversarial radio.
- **Touch is not wired up.** CST816S pins are declared in
  `pincfg.h` but no driver is bound yet. BOOT button is the only
  input.
- **No IMU / RTC / battery.** The JC3636 board has none of these.
  Status ack reports `bat.pct:100, mA:0, usb:true` as placeholder
  values; shake / face-down / clock features from the M5 version
  are not ported.

## Project layout

```
src/
  ble_bridge.h                          ← shared public header
  ble_bridge.cpp                        ← Bluedroid version (M5 env, unused here)
  jc3636/
    main_jc3636.cpp                     ← setup/loop, UI composition
    display.c / display.h               ← ST77916 QSPI panel + LEDC backlight
    canvas.h / canvas.cpp               ← TFT_eSPI-shaped RGB565 sprite + 6×8 font
    font6x8.h                           ← Adafruit_GFX glcdfont (Apache-2.0)
    buddy_blob.h / buddy_blob.cpp       ← blob species, 7 state functions
    ble_bridge_nimble.cpp               ← NimBLE NUS server, JustWorks pairing
    pincfg.h                            ← board pin map
    vendor/                             ← esp_lcd_st77916 driver (Apache-2.0)
boards/
  jc3636w518en.json                     ← PlatformIO board definition
platformio.ini                          ← [env:jc3636w518en] + upstream [env:m5stickc-plus]
REFERENCE.md                            ← unchanged BLE protocol spec from upstream
```

## Credits & license

- Upstream firmware, BLE protocol, and original pet art:
  [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
  (© 2026 Anthropic, PBC — MIT).
- Bitmap font: Adafruit GFX Library `glcdfont.c` (Apache-2.0).
- ST77916 panel driver: Espressif `esp_lcd_st77916` component
  (Apache-2.0) — vendored under `src/jc3636/vendor/`.

This port is released under the same **MIT License** as the upstream
project. See [LICENSE](LICENSE).
