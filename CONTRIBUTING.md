# Contributing

Thanks for considering a contribution. This firmware is beta — the
BYOD protocol implementation is solid, but corners of the hardware
story (additional panels, power-rail variants, button layouts on
non-X3 boards) haven't been exercised yet. Bug reports and patches
are welcome.

## What this project is (and isn't)

**Is:** a TRMNL BYOD-compatible firmware for the Xteink X3, built on
the CrossPoint Reader HAL. It speaks the standard TRMNL wire format
(`/api/setup`, `/api/display`, `/api/log`) well enough to work against
every maintained BYOS implementation.

**Isn't:** a general-purpose ESP32 framework, a fork of the reference
TRMNL firmware (we rebuilt the device side fresh against the BYOD
spec), or a replacement for CrossPoint Reader's book-reader features
(those are stripped out by design). See [CREDITS.md](CREDITS.md) for
upstream attribution.

## Areas where help is especially welcome

- **Security review.** The BYOD contract has no transport auth beyond
  a bearer token over TLS, and OTA has no signature verification. The
  firmware uses `setInsecure()` (no cert pinning) and relies on URL
  dedup to prevent OTA loops. An independent look at the threat model
  would be valuable.
- **Stability.** Especially on boards/panels we haven't tested.
  Crash traces (`.panic` → `scripts/debugging_monitor.py`) are gold.
- **Power audit.** `POWER.md` is the operational doc; if you can
  measure the real wake cycle with a µCurrent or scope and find
  regressions from the numbers we document, we want to know.
- **Additional BYOS compatibility.** See
  [`docs/byod-compat.md`](docs/byod-compat.md). Any failure against a
  listed server (or a new one) is a bug.
- **Panel support.** The display / framebuffer path assumes 792×528.
  Adding a config for other SSD1677-family panels should be
  straightforward; see [`docs/hardware.md`](docs/hardware.md).

## Ground rules

1. Don't regress the deep-sleep / timer-wake path on battery. The
   GPIO13 HIGH-hold is the single most load-bearing piece of this
   firmware — without it, unattended refresh stops working. Every
   change to `TrmnlActivity::runCycleAndSleep` or `startTimerSleep`
   needs a before/after cycle on a battery-only device.
2. No blocking work on the render path without a sleep exit. Any new
   branch must eventually hit `display.deepSleep()` +
   `startTimerSleep()` — otherwise the device stays awake and the
   battery dies in days.
3. Keep the BYOD protocol implementation to the *intersection* of
   what maintained BYOS servers speak, not to any single server's
   superset. When in doubt, check `docs/byod-compat.md`.
4. CrossPoint upstream licensing is MIT; any new code should stay
   compatible.

## Development workflow

```bash
git clone --recursive https://github.com/<user>/x3-trmnl
cd x3-trmnl
cp platformio.example.ini platformio.local.ini    # edit for your setup
pio run
scripts/flash.sh                                   # or use the web flasher
```

For iterating without hardware: `pio check` runs static analysis,
`pio run` without `-t upload` just validates the build.

## Submitting changes

1. Open an issue first for non-trivial changes so we can discuss
   scope.
2. Keep commits focused. `feat(api): add X` / `fix(image): handle Y`
   / `docs(byod-compat): clarify Z` — conventional-commit prefixes
   help us skim the history.
3. Build clean before pushing: `pio run` must succeed at each
   commit (not just at the tip of the PR).
4. If you touch hardware-adjacent code, flash and verify on a real
   device. Attach a serial log to the PR.
5. Open the PR against `main`. Expect a turnaround of days-to-weeks
   — this is a weekend project, not a full-time one.

## Code style

- Match the surrounding file. The CrossPoint HAL uses camelCase,
  PascalCase for classes, `UPPER_SNAKE` for macros.
- C++20, no exceptions, no RTTI (matching CrossPoint / ESP-IDF
  defaults).
- Arduino `String` / `std::string` both appear; prefer `std::string`
  in new code except where Arduino APIs require `String`.
- Comments explain *why*, not *what*. The CrossPoint code sets a
  good example; follow it.
