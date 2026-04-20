# Flashing

There are three viable paths for getting firmware onto an X3. The
browser flasher is the least painful; the CLI script is what you want
when you're iterating locally.

## Recommended: browser flasher (app partition only)

<https://x3.crosspointreader.com> uses WebSerial + `esptool-js` in the
browser. It is maintained by the CrossPoint Reader team, matches the
upload behaviour the X3 USB-Serial-JTAG peripheral expects, and handles
the dual-OTA partition switch correctly.

**Scope:** writes only the app partition. Bootloader and partition
table on-device are not touched. That is usually what you want: this
firmware's `partitions.csv` matches CrossPoint's, so if the X3 was
ever flashed with CrossPoint (or Xteink stock, which shares the same
partition layout) you never need to rewrite those.

1. Download `firmware.bin` from the Releases page (or build it with
   `pio run`).
2. Connect the X3 over USB-C.
3. Chrome/Edge/Arc → <https://x3.crosspointreader.com>.
4. *Flash firmware from file* → select `firmware.bin`.
5. The tool writes to the inactive OTA slot and flips `otadata` so
   the device boots into the new image on restart — no manual
   partition fiddling.

If you need to rewrite the bootloader or partition table too (because
you are coming from a completely different firmware, or recovering
from a brick), use the CLI path below.

## CLI: `scripts/flash.sh`

Use this when you're on a headless machine or can't run Chrome.

```bash
pio run                       # build
scripts/flash.sh              # full flash (bootloader + partitions + app)
scripts/flash.sh firmware-only  # faster: rewrite app partition only
scripts/flash.sh erase        # wipe flash entirely (destructive — use if stuck)
```

The script busy-waits up to 90 s for a USB serial device to appear,
then invokes esptool. This is necessary because the X3 port only
enumerates while the chip is awake; tap the power button once while
the script is waiting.

Override the timeout or connect attempts:

```bash
FLASH_WAIT_SECS=30 ESPTOOL_CONNECT_ATTEMPTS=50 scripts/flash.sh
```

## CLI: raw `pio run -t upload`

Works too, but you lose the busy-wait — PlatformIO times out in ~5 s if
the port isn't there when upload starts.

```bash
pio run -t upload --upload-port /dev/cu.usbmodem2101
```

## What "port locked" looks like

Every variant of `The chip stopped responding`, `Serial data stream
stopped`, `Packet content transfer stopped`, and `Invalid head of
packet` is almost always caused by *something else holding the serial
port*. PlatformIO's monitor, a previous Python `serial.Serial(...)`,
Minicom, the browser flasher tab still open — any of them will corrupt
the SLIP frames esptool sends.

```bash
lsof /dev/cu.usb*             # find the holder
kill -9 <pid>                 # free the port
```

After the port is free the normal flash cycle completes in under 10
seconds.

## First-time provisioning

After a fresh flash the device has no WiFi credentials and no api_key,
so it boots directly into the captive portal. From any phone:

1. Join the open AP `TRMNL-<mac-suffix>`
2. The portal page at `http://192.168.4.1` opens automatically (all
   major captive-portal probes redirect to it)
3. Fill in SSID + password, and optionally a custom server URL
4. *Save & reboot*
5. On the next wake the firmware runs `GET /api/setup`, receives an
   `api_key` and `friendly_id`, renders the setup splash, and sleeps

If the server doesn't know this MAC it will either auto-provision
(Terminus, LaraPaper, Inker, BYOS Next.js / FastAPI) or refuse
(Django, Phoenix — those need the MAC seeded in the server DB first,
check the respective BYOS docs).

## Re-entering the captive portal

Three ways, in order of friendliness:

| Method | When |
|---|---|
| Hold **bottom-left front button (BACK)** while tapping power | Anytime the device is asleep; works in the field |
| Change the server URL in the portal and save | Clears api_key + friendly_id, re-provisions against the new host |
| Serial `CMD:WIPE` (dev builds with `-DENABLE_SERIAL_LOG`) | When you're attached over USB anyway |

The BACK-at-boot path polls the button for ~5 s after power-on; hold
BACK, *then* tap power, and keep BACK held until the portal splash
appears.

## If the device is truly stuck

`scripts/flash.sh erase` full-chip erases. On the next power-on the
ROM bootloader will find no valid bootloader and wait for an upload
over USB, so you can re-flash fresh. NVS (stored WiFi creds, api_key,
friendly_id) is destroyed — the device comes up in the captive portal
from scratch.
