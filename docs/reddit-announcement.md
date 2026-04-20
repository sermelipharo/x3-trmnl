lex# Reddit / forum announcement draft

Post this once the GitHub repo is public. Suggested subreddits:
r/selfhosted (main), r/esp32, r/eink. Mention the TRMNL BYOD /
BYOS community channels too.

---

## Title

> I built a TRMNL BYOD firmware for the Xteink X3 — early beta, looking for testers (especially for security / stability / power review)

## Body

I own a [TRMNL](https://usetrmnl.com/)-ish e-ink dashboard that runs on the
Xteink X3 (ESP32-C3 + 792×528 e-ink), and couldn't find a firmware that
both (a) speaks the standard TRMNL [BYOD](https://docs.trmnl.com/go/diy/byod)
protocol and (b) runs on the X3's specific hardware. So I wrote one.

**Repo:** `https://github.com/<user>/x3-trmnl`  *(link once public)*

### What it is

A fresh TRMNL BYOD client built on top of the excellent
[crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
firmware's hardware-abstraction layer. The CrossPoint team figured out
the X3's display driver, power topology, MOSFET-latched battery rail,
button decoding, and flash layout — I stripped their EPUB-reader stack
and dropped in a TRMNL display loop where the reader activities used
to live. All upstream copyright belongs to them; the BYOD wire
contract is TRMNL's.

One binary talks to any maintained BYOS server — LaraPaper, Terminus,
Inker, BYOS Next.js, BYOS FastAPI, BYOS Django, BYOS Phoenix — plus
the reference TRMNL cloud. Captive portal on first boot collects
WiFi + server URL; `/api/setup` auto-provisions an api_key; wake →
fetch → render → deep sleep cycle on a configurable interval.

Smoke-tested end-to-end against LaraPaper and BYOS Next.js on real
hardware. The dual-OTA partition layout matches upstream, so
OTA-via-`update_firmware` works (with URL dedup so a misbehaving
server can't reflash you every wake).

### What it is NOT

**Do not flash this as your daily driver.** This is raw beta. I run
it on one dev unit, I've banged on the common paths, but I have not:

- Had anyone audit it for security (TLS uses `setInsecure()`, OTA
  is unsigned — the BYOD spec itself doesn't include signatures).
- Run it for weeks unattended to prove the timer-wake path really
  is as stable as it looks.
- Measured the actual current profile in the field — the sleep
  bookkeeping and the GPIO13 latch are tuned but not
  instrumented.

If you want a mature firmware for the X3, use
[crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader).
If you want to help test a TRMNL firmware for it and you're OK with
reflashing when things break, read on.

### What I specifically need eyes on

1. **Security.** Threat model is "trusted home LAN to trusted BYOS
   instance." TLS is unverified (chain isn't pinned), api_key is
   bearer-only, OTA has no signature. If any of that is wrong for a
   particular deployment I'd rather know early. PRs for cert
   pinning / signed OTA welcome.
2. **Stability.** The device is on battery with a MOSFET-gated rail.
   Unattended timer-wake only works because we latch GPIO13 HIGH
   through deep sleep — if someone finds a case where that breaks
   (weird edge-case reset, silicon erratum, particular USB plug-in
   sequence), it's a real problem.
3. **Power.** The `POWER.md` doc lists the awake-time budget we
   aim for (~7–8 s per cycle, ~62 µAh on cache-hit). Real
   measurements would confirm or refute.
4. **BYOS compatibility.** I've tested against LaraPaper and BYOS
   Next.js. If anyone runs Terminus / Inker / the other BYOS
   variants on real TRMNL-X3 hardware, I'd love a report.

### How to try it (if you accept the above)

- Build with PlatformIO (`pio run`) or grab a `firmware.bin` from
  the Releases page once they exist.
- Flash via <https://x3.crosspointreader.com> (browser flasher,
  recommended) or the `scripts/flash.sh` helper.
- Hold the bottom-left front button while tapping power on a
  flashed unit to force the captive portal and re-provision.

See the docs for [flashing](docs/flashing.md),
[hardware notes](docs/hardware.md), and the
[BYOS compatibility matrix](docs/byod-compat.md).

### Feedback channels

- GitHub issues on the repo (preferred — include serial logs)
- PRs always welcome; see [CONTRIBUTING.md](CONTRIBUTING.md)

Massive thanks to the CrossPoint Reader team for the base firmware,
and to TRMNL for shipping an open BYOD spec in the first place.
