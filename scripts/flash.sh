#!/usr/bin/env bash
# CLI flasher for the X3 TRMNL firmware.
#
# The X3's USB-Serial-JTAG port appears and disappears with the device's
# deep-sleep state, so this script busy-waits for a /dev/cu.usbmodem*
# (or /dev/ttyACM* on Linux) to appear and invokes esptool as soon as it
# does. Use it when the browser flasher at
# https://x3.crosspointreader.com isn't available.
#
# Usage:
#   scripts/flash.sh                          # flash bootloader + partitions + firmware.bin
#   scripts/flash.sh firmware-only            # flash just the app at 0x10000
#   scripts/flash.sh erase                    # erase entire flash (last resort)
#
# Troubleshooting:
#   - "chip stopped responding" or "Serial data stream stopped" after
#     handshake usually means something else has the port open. Kill any
#     zombie readers: lsof /dev/cu.usb*  then  kill <pid>
#   - If the port never appears, tap the power button to wake the device
#     first. Device sleeps rapidly between wakes so you may need to hold
#     it or reflash during the ~30 s awake window.
#   - To force the captive portal after flashing, hold the bottom-left
#     front button (BACK) while tapping power — the bootloader polls
#     BACK for ~5 s.
set -euo pipefail

MODE="${1:-full}"
WAIT_SECS="${FLASH_WAIT_SECS:-90}"
CONNECT_ATTEMPTS="${ESPTOOL_CONNECT_ATTEMPTS:-30}"

BUILD_DIR="$(cd "$(dirname "$0")/../.pio/build/default" && pwd)"
BOOTLOADER="$BUILD_DIR/bootloader.bin"
PARTITIONS="$BUILD_DIR/partitions.bin"
FIRMWARE="$BUILD_DIR/firmware.bin"

for f in "$BOOTLOADER" "$PARTITIONS" "$FIRMWARE"; do
  if [[ ! -f "$f" ]]; then
    echo "Missing $f — run 'pio run' first." >&2
    exit 2
  fi
done

# Locate esptool. PlatformIO ships one; fall back to system esptool.
if command -v pio >/dev/null 2>&1; then
  ESPTOOL_CMD=(pio pkg exec -- esptool.py)
elif command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL_CMD=(esptool.py)
elif command -v esptool >/dev/null 2>&1; then
  ESPTOOL_CMD=(esptool)
else
  echo "Could not find esptool. Install PlatformIO or 'pip install esptool'." >&2
  exit 2
fi

find_port() {
  # macOS first, then Linux.
  find /dev -maxdepth 1 \( -name 'cu.usbmodem*' -o -name 'ttyACM*' \) 2>/dev/null | head -1
}

echo "=== waiting up to ${WAIT_SECS}s for device ==="
echo "   tap power button now if the screen is off"
deadline=$(( $(date +%s) + WAIT_SECS ))
PORT=""
while [[ $(date +%s) -lt $deadline ]]; do
  PORT="$(find_port)"
  [[ -n "$PORT" ]] && break
done
if [[ -z "$PORT" ]]; then
  echo "Timed out waiting for port. Device still asleep?" >&2
  exit 3
fi
echo "=== port=$PORT ==="

case "$MODE" in
  full)
    "${ESPTOOL_CMD[@]}" \
      --port "$PORT" --chip esp32c3 \
      --before default-reset --after hard-reset \
      --connect-attempts "$CONNECT_ATTEMPTS" \
      write_flash \
      0x0 "$BOOTLOADER" \
      0x8000 "$PARTITIONS" \
      0x10000 "$FIRMWARE"
    ;;
  firmware-only|app)
    "${ESPTOOL_CMD[@]}" \
      --port "$PORT" --chip esp32c3 \
      --before default-reset --after hard-reset \
      --connect-attempts "$CONNECT_ATTEMPTS" \
      write_flash 0x10000 "$FIRMWARE"
    ;;
  erase)
    echo "Erasing entire flash — this will wipe all OTA partitions + NVS." >&2
    "${ESPTOOL_CMD[@]}" \
      --port "$PORT" --chip esp32c3 \
      --before default-reset --after hard-reset \
      --connect-attempts "$CONNECT_ATTEMPTS" \
      erase_flash
    ;;
  *)
    echo "Unknown mode: $MODE (expected: full | firmware-only | erase)" >&2
    exit 2
    ;;
esac
