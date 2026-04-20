# Credits

This project is a fork/derivative work. The original code and ideas belong
to their upstream authors — this repo adds a TRMNL BYOD client layer on
top and reuses their hardware abstraction, display driver, input layer,
and build system.

## Upstream projects

### [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)

This firmware is a hard fork of the CrossPoint Reader project. We inherit:

- `lib/hal/` — the `HalDisplay`, `HalGPIO`, `HalStorage`, `HalPowerManager`,
  `HalSystem` abstractions
- `lib/EpdFont/`, `lib/GfxRenderer/` — font rendering and framebuffer
  blitting
- `open-x4-sdk/` — the low-level SDK submodule with
  `EInkDisplay`, `InputManager`, `BatteryMonitor`, `SDCardManager`
- `src/activities/` — the Activity lifecycle + `ActivityManager`
- `src/CrossPointSettings`, `src/CrossPointState` — settings/state
  persistence (kept, even though they no longer track reader state, so
  the upgrade path from a CrossPoint install is non-destructive)
- the PlatformIO build setup and the X3-specific GPIO13 MOSFET latching
  that makes timer-wake survive on battery

All of that is licensed MIT by the CrossPoint Reader authors. The
original LICENSE and copyright notices are preserved in-tree.

We stripped the reader-specific stack (EPUB parsing, book metadata
cache, chapter rendering, file browser, KOReader sync, WebDAV, OPDS)
to make room for the TRMNL display loop. See the git history for the
exact deletions.

### [TRMNL](https://usetrmnl.com/) + [docs.trmnl.com](https://docs.trmnl.com)

The BYOD (Bring-Your-Own-Device) and BYOS (Bring-Your-Own-Server)
protocols this firmware speaks are defined by TRMNL. Specifically:

- [docs.trmnl.com/go/diy/byod](https://docs.trmnl.com/go/diy/byod) — the
  device-side wire format we implement
- [docs.trmnl.com/go/diy/byod-s](https://docs.trmnl.com/go/diy/byod-s) —
  server-side notes we used to write the captive portal's "server URL"
  field
- The [usetrmnl/firmware](https://github.com/usetrmnl/firmware) reference
  implementation — canonical source for header names and response field
  semantics when docs were ambiguous

The BYOD HTTP contract itself is not copyrightable, but we owe the
interface design + the ecosystem of BYOS implementations (Terminus,
LaraPaper, Inker, BYOS Next.js / FastAPI / Django / Phoenix) to TRMNL.

### [xteink-flasher](https://github.com/crosspoint-reader/xteink-flasher) / [x3.crosspointreader.com](https://x3.crosspointreader.com)

The browser-based flasher this README points at as the recommended
install path is maintained by the CrossPoint Reader team.

## License

This repo is MIT, matching upstream CrossPoint. See `LICENSE` for the
full text — the copyright line covers our additions; the underlying
CrossPoint and third-party library notices are preserved in their own
files.
