# X3 hardware notes

What we know about the Xteink X3 board from fighting with it during
this port. Mostly sourced from the CrossPoint Reader HAL and confirmed
on hardware; take pin numbers with a grain of salt if you're reading
these for a different revision.

## MCU + memory

- **ESP32-C3** (RISC-V single-core @ 160 MHz, revision v0.4)
- 16 MB SPI flash (partition table at `0x8000`, dual-OTA apps at
  `0x10000` / `0x650000`, LittleFS at `0xc90000`)
- ~320 KB usable DRAM. No PSRAM. Single-buffer e-ink mode (one 48 KB
  framebuffer, not double-buffered) is mandatory to have any DRAM left.

## Display

- **SSD1677-family driver** over SPI (`SPI_MOSI`, `SPI_CLK`,
  `SPI_CS`, `DC`, `RST`, `BUSY` per the HAL), 10 MHz clock.
- Panel is **792 × 528**, 1-bpp. Visible bezel on each edge is
  ~1–2 pixels — confirmed by rendering a nested-L calibration pattern
  with insets `{0, 2, 4, …, 38}`: at inset 0 the L falls behind the
  bezel, every other inset is fully visible.
- Refresh rates: **FULL** ~1.6 s (clears ghosting), **FAST** ~0.6 s.
  The firmware does a FULL refresh once every 16 wakes, FAST on every
  other cache-miss cycle.

## Power

This is the most load-bearing part of the port and the reason we
couldn't just reuse CrossPoint's sleep logic verbatim.

- USB VBUS and battery feed a single rail into the MCU via a
  charger/PMIC. The MCU's `VCC` is **gated by a MOSFET** whose gate is
  driven from `GPIO13`.
- Default gate state leaves the MOSFET **open** (rail cut) to minimise
  standby. The CrossPoint reader used this to cut power completely
  during deep sleep — but that also means the RTC dies, and the device
  can only wake on a power-button press.
- For unattended timer-wake (what TRMNL needs), we drive
  `GPIO13 = HIGH` and latch it via `gpio_hold_en(13)` +
  `gpio_deep_sleep_hold_en()` before `esp_deep_sleep_start()`. The
  latch survives EN reset and deep sleep, so the MCU (and its RTC)
  stays powered continuously. The trade-off is ~5–10 µA extra
  quiescent; in practice timer-wake works reliably on battery.
- On EN reset (physical reset or `esp_restart()`) the GPIO-hold latch
  persists, so reset does **not** drop the rail. On power-on reset
  from a fully-drained state the gate floats and the MOSFET opens —
  then USB VBUS has to bring things up first, and the new firmware
  re-asserts GPIO13 HIGH early.

The consequence of this for flashing: if the running app holds
`GPIO13 = HIGH`, attaching an external flasher usually works. But if
you manage to interrupt the firmware so it doesn't re-assert GPIO13
(e.g. ROM bootloader running but app never boots), the MOSFET can
drop mid-operation and USB vanishes. That's the root of every
"serial data stream stopped" we hit in development.

## Buttons

Two ADC ladders + one GPIO for power, decoded by
`open-x4-sdk/libs/hardware/InputManager`. Physical → logical mapping,
discovered by flashing a `TRMNL_BUTTON_DIAG=1` build and pressing each
button in sequence:

| Physical position      | Logical `BTN_*` | InputManager index |
|------------------------|-----------------|--------------------|
| Side, left             | `BTN_UP`        | 4                  |
| Side, right            | `BTN_DOWN`      | 5                  |
| Front, bottom-left 1   | `BTN_BACK`      | 0                  |
| Front, bottom-left 2   | `BTN_CONFIRM`   | 1                  |
| Front, bottom-right 1  | `BTN_LEFT`      | 2                  |
| Front, bottom-right 2  | `BTN_RIGHT`     | 3                  |
| Power button           | `BTN_POWER`     | 6                  |

The four front buttons share one ADC ladder (`BUTTON_ADC_PIN_1 = GPIO1`),
the two side buttons share another (`BUTTON_ADC_PIN_2 = GPIO2`), and
the power button is a plain digital input on `GPIO3` (pulled up, low
when pressed).

Hold `BTN_BACK` (bottom-left 1) while tapping power to force the
captive portal at boot — `src/main.cpp` polls it for 5 s.

## Fuel gauge

- **BQ27220** over I²C (`X3_I2C_SDA`, `X3_I2C_SCL` — see
  `lib/hal/HalGPIO.h`). The firmware reads voltage (register `0x08`)
  and state-of-charge (register `0x2C`) once per wake and reports
  both in the `Battery-Voltage` header.
- `HalPowerManager::getBatteryVoltageMV()` returns 0 when the chip
  hasn't booted yet or the I²C transaction failed; we fall back to
  4.0 V in that case so the server side doesn't see absurd readings.

## USB

- Native **USB-Serial-JTAG** on `GPIO18`/`GPIO19`. No external USB-UART
  bridge. VID:PID = `0x303A:0x1001`.
- HWCDC is used for both logs and command input in dev builds
  (`CMD:WIPE`, `CMD:REFRESH`, `CMD:STATUS` — see `src/main.cpp`).
- Because HWCDC shares the CDC endpoint with the ROM bootloader, any
  other host-side process holding `/dev/cu.usbmodem*` open while
  esptool is trying to talk to it will corrupt the SLIP framing. Kill
  stray readers before flashing.
