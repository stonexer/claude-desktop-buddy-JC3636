# Contributing

PRs welcome. This is a community port, not an official Anthropic project
— the upstream [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
explicitly asks you to fork rather than PR, but the JC3636 port here
*does* take contributions.

## Good first PRs

- Wire up the CST816S touch driver so tap/swipe can drive UI actions.
- Port more species from `upstream/src/buddies/` into the
  Canvas-shaped renderer.
- Add a CLI-side bridge so `claude` terminal sessions can drive the
  pet without the Desktop app being in the loop.
- Shrink the flash footprint (currently ~700 KB of a 2 MB app partition).

## Keeping in sync with upstream

The BLE wire protocol lives in [REFERENCE.md](REFERENCE.md) and should
stay verbatim with upstream. If upstream publishes a protocol bump,
copy the updated file over instead of editing in place — anything
that drifts off-spec stops working with the Desktop bridge.

Platform-specific code lives under `src/jc3636/`. The M5 env in
`platformio.ini` is kept for reference but is not expected to build
on current pioarduino (its upstream Arduino-ESP32 no longer ships the
`m5stick-c` variant).

## Local dev

```bash
pio run -e jc3636w518en -t upload
```

If the device gets stuck mid-upload, hold BOOT while re-plugging USB
to force ROM download mode.
