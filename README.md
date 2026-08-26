# TRMNL BYOD firmware

> [!IMPORTANT]
> **The X3 panel is 792×528, not the TRMNL-standard 800×480.**
> Most BYOS servers (and the TRMNL cloud) default to 800×480 and will
> send images that overflow the panel by 8 px on the right edge. Before
> the device provisions, create a device record on your server with
> the X3 dimensions so it gets correctly-sized bitmaps:
>
> | Field            | Value                                                |
> |------------------|------------------------------------------------------|
> | `mac_address`    | device MAC — shown on the captive-portal splash, or printed on the back of the unit |
> | `screen_width`   | `792`                                                |
> | `screen_height`  | `528`                                                |
> | `bit_depth` / `grayscale` | `1` (1-bpp)                                 |
> | `model`          | matches `TRMNL_DEVICE_MODEL` — `byod` by default     |
>
> Exact field names vary per server: BYOS Next.js has
> `screen_width` / `screen_height` on the `devices` table (set before
> the first `/api/setup`, or via the admin UI); LaraPaper has a
> `DeviceModel` record (`width`, `height`, `scale_factor`, `bit_depth`);
> Terminus / Inker / BYOS FastAPI have similar fields in their admin
> surfaces. If you skip this, the firmware will still render — it clips
> oversized frames at write time — but content anchored to the right
> edge will be cut off.

A [TRMNL BYOD](https://docs.trmnl.com/go/diy/byod)-compatible firmware
for the **Xteink X3** e-paper reader (ESP32-C3, 792×528 e-ink). One
binary works against any TRMNL Bring-Your-Own-Server implementation —
LaraPaper, Terminus, Inker, BYOS Next.js / FastAPI / Django / Phoenix —
and against the reference TRMNL cloud.

Built on the [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
hardware abstraction layer; the reader-specific stack (EPUB parsing,
book cache, reader UI) has been stripped down to the essentials the
wake → fetch → render → sleep cycle needs.

## Status — raw beta, do not use as a daily driver

**This is early beta software.** Do not flash it onto an X3 you rely
on. The BYOD protocol implementation is functional (captive-portal
provisioning, `/api/setup`, `/api/display`, `/api/log`, OTA via
`update_firmware`, server-commanded `reset_firmware`) and has been
smoke-tested end-to-end against LaraPaper and BYOS Next.js on real
hardware — but it hasn't been audited for security, stability has
not been verified across long unattended runs, and the power profile
is unmeasured in the field.

The original CrossPoint Reader firmware remains the mature, stable
option for X3 hardware; switch back via
<https://x3.crosspointreader.com> if anything here misbehaves.
Contributions — especially around security, stability, and power —
are specifically invited; see [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

This is a derivative work. The hardware abstraction layer, e-ink
driver, font system, build setup, and X3-specific sleep logic come
from [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader).
The BYOD/BYOS protocol is defined by [TRMNL](https://usetrmnl.com/)
and documented at [docs.trmnl.com](https://docs.trmnl.com/go/diy/byod).
All upstream copyrights belong to their respective authors. See
[CREDITS.md](CREDITS.md) for full attribution.

## Hardware

- **Xteink X3** (ESP32-C3 RISC-V, 16 MB flash, ~320 KB DRAM)
- **UC8253 or UC8279d e-ink controller** (production-batch dependent),
  792×528, 1-bpp; detected automatically at boot
- **BQ27220** fuel gauge over I²C
- MOSFET-off battery latch controlled by GPIO13. The timer-wake path
  holds GPIO13 HIGH through deep sleep so the MCU's RTC stays powered
  on battery; this is the only reason unattended refresh works
  without an external RTC.

Other ESP32-C3 hardware with a compatible e-ink panel can be adapted
by editing the HAL layer under `lib/hal/` and `freeink-sdk/`.

## BYOD protocol compliance

The firmware implements the [BYOD device-side
contract](https://docs.trmnl.com/go/diy/byod) with the intersection
of headers and response fields that every maintained BYOS speaks.

**Headers sent**

- `ID` — device MAC
- `Access-Token` — api_key from `/api/setup` (omitted pre-provisioning)
- `FW-Version`
- `Model` — required by BYOS Next.js for auto-provisioning; others
  accept it silently
- `Battery-Voltage`, `RSSI`, `Refresh-Rate`, `Content-Type`

**Response fields handled**

- `status` (0 or 200 = OK, 202 = awaiting link, 5xx = server error)
- `image_url`, `filename`, `refresh_rate`
- `reset_firmware` → wipe credentials and reboot into captive portal
- `update_firmware` + `firmware_url` → OTA via the Arduino Update
  library, gated on a `lastOtaUrl` NVS dedup so a misbehaving server
  cannot reflash every wake

**Image formats**

- BMP3 1-bpp, any dimensions up to the panel (what Terminus,
  LaraPaper, Inker, BYOS Next.js / Phoenix serve primarily)
- PNG 1 / 2 / 4 / 8-bpp grayscale or palette; hand-rolled decoder
  clips oversized frames (upstream-standard 800×480 on a 792×528
  panel) at write time and leaves uncovered regions white

## Provisioning flow

1. Fresh flash → device boots into the captive portal, advertises
   an open AP named `TRMNL-<MAC suffix>`
2. User joins the AP; the portal form at `http://192.168.4.1` takes
   WiFi SSID + password and an optional server URL
3. Empty server URL falls back to `TRMNL_DEFAULT_SERVER_URL` at
   build time (defaults to empty — production builds require the
   user to enter one)
4. On the next boot the device has WiFi + server URL, runs
   `GET /api/setup` with its MAC, receives an `api_key` +
   `friendly_id`, persists both to NVS, and renders the setup splash
5. Subsequent wakes hit `/api/display`, compare `filename` against
   the cached one, and skip download + decode on cache hit

## Re-entering the captive portal

- **Hold the bottom-left front button (BACK) while tapping power**
  — the device polls for ~5 s at boot and enters the portal if it
  sees BACK held.
- **Serial `CMD:WIPE`** (dev builds with `-DENABLE_SERIAL_LOG`) —
  wipes WiFi + api_key + friendly_id, reboots into the portal.
- Changing the server URL in the portal clears the api_key so the
  next boot re-runs `/api/setup` against the new host.

## Configuration

All runtime values live in NVS (see `src/trmnl/TrmnlState.cpp`).
Build-flag defaults only seed *uninitialized* NVS slots; once a
value has been written (even to empty, e.g. when the captive portal
clears the api_key on a server-URL change) the stored value wins.

### Build flags (platformio.ini or platformio.local.ini)

| Flag                              | Default | Purpose                                                                                                     |
|-----------------------------------|---------|-------------------------------------------------------------------------------------------------------------|
| `TRMNL_DEFAULT_SERVER_URL`        | empty   | Pre-fills the captive-portal server URL                                                                     |
| `TRMNL_DEFAULT_API_KEY`           | empty   | Pre-provisions an api_key for an already-registered device                                                  |
| `TRMNL_DEVICE_MODEL`              | `byod`  | Reported in the Model header                                                                                |
| `TRMNL_DEFAULT_REFRESH_SECONDS`   | 1800    | Sleep interval when the server omits `refresh_rate`                                                         |
| `TRMNL_FIRMWARE_VERSION`          | `0.1.0` | Reported in the FW-Version header                                                                           |
| `TRMNL_WIFI_CONNECT_TIMEOUT_MS`   | 15000   | Per-attempt WiFi connect timeout (ms)                                                                       |
| `TRMNL_BOOTSTRAP_WIFI_SSID`       | —       | Optional: seed stored WiFi creds without going through the portal                                           |
| `TRMNL_BOOTSTRAP_WIFI_PASSWORD`   | —       | Optional: paired with the SSID above                                                                        |
| `TRMNL_DISABLE_OTA`               | unset   | Define to refuse server-requested firmware updates regardless of `update_firmware` in the response          |
| `TRMNL_BUTTON_DIAG`               | unset   | Dev: replace the normal boot flow with an infinite button-diagnostic loop that logs presses to serial       |

Copy `platformio.example.ini` to `platformio.local.ini` (gitignored)
and set whichever flags you need for your build environment.

### Serial commands (dev builds)

When the firmware is built with `-DENABLE_SERIAL_LOG`, the device
accepts commands on its USB CDC interface during the short awake
window after each wake:

- `CMD:WIPE` — wipe WiFi creds + set the captive-portal flag, reboot
- `CMD:REFRESH` — clear the cached filename, reboot (forces a
  re-render on the next cycle)
- `CMD:STATUS` — dump current credential count, portal flag, wake
  count, and last filename to the console

## Building

```
pip install -U platformio
git submodule update --init --recursive
cp platformio.example.ini platformio.local.ini   # edit as needed
pio run
```

## Flashing

Grab the artifacts from the
[Releases page](https://github.com/sermelipharo/x3-trmnl/releases) —
each tagged build ships `firmware.bin`, `bootloader.bin`,
`partitions.bin`, and `SHA256SUMS.txt`. For normal updates you only
need `firmware.bin`.

**Browser flasher (recommended for updates).** Open
<https://x3.crosspointreader.com> in Chrome / Edge / Arc, click
*Flash firmware from file*, and select the `firmware.bin` you
downloaded. It uses WebSerial + `esptool-js`, writes the image to
the inactive OTA partition, and flips `otadata` on success. This
path **only updates the app partition** — the bootloader and
partition table on-device are untouched.

That's enough for every normal update because this firmware's
`partitions.csv` is the same as CrossPoint's (dual-OTA at `0x10000`
/ `0x650000`) and the CrossPoint bootloader is compatible. If your
X3 shipped with CrossPoint or the stock Xteink firmware, the
browser flasher is all you need.

**CLI flasher (bootloader + partitions + app, or recovery).** If
the device is coming from a completely different firmware, or if
you need a full recovery flash after something went wrong, use
`scripts/flash.sh`:

```
scripts/flash.sh              # full flash (bootloader + partitions + app)
scripts/flash.sh firmware-only  # app partition only; equivalent to the browser flasher
scripts/flash.sh erase        # wipe the entire flash (last resort)
```

The script busy-waits for the port to appear (the X3 only enumerates
while awake — tap the power button once after starting). Raw
`pio run -t upload` works too; if it fails mid-handshake, check for
background processes holding the port (`lsof /dev/cu.usb*`) — a
stuck serial reader will corrupt SLIP frames in ways that look like
hardware faults.

See [`docs/flashing.md`](docs/flashing.md) for the full recipe,
first-time provisioning, how to re-enter the captive portal, and
recovery if something bricks.

## Known gaps

- OTA has no signature verification yet — the BYOD contract itself
  does not include one. The URL-dedup mitigates the "server always
  flags update" loop; for defence-in-depth deploy behind TLS-pinned
  infrastructure or build with `-DTRMNL_DISABLE_OTA`.
- `/api/log` is fire-and-forget with an ad-hoc payload; the spec
  leaves the body schema undefined.
- The HAL currently assumes the X3 panel (792×528). Other panels
  require edits to `lib/hal/HalDisplay` and the EPD driver.

## License

MIT — see `LICENSE`. Derived from crosspoint-reader (also MIT).
