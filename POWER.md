# X3 TRMNL — Power Efficiency Research & Audit

Produced by the power-efficiency research agent on 2026-04-18. Source of truth for power-related decisions on this firmware.

**Scope:** ESP32-C3 + Xteink X3 e-paper as a TRMNL dashboard (deep-sleep duty-cycle device). The tiebreaker for every decision below is maximum battery life, not code cleanliness.

**Status of codebase:** This repo is currently CrossPoint (e-reader) being re-targeted to TRMNL on branch `x3-trmnl`. No TRMNL networking or duty-cycle code exists yet. Cited figures for the render/display path come from the existing CrossPoint/EInkDisplay code; figures for the not-yet-written TRMNL path are design targets.

**Hardware baseline:**
- ESP32-C3 (single-core RISC-V, 160 MHz max, no PSRAM, ~380 KB SRAM, 8 KB RTC_SLOW_MEM)
- Xteink X3: SSD1677-family controller, 792×528 1bpp (≈52 KB framebuffer), **10 MHz SPI** (see `open-x4-sdk/libs/display/EInkDisplay/src/EInkDisplay.cpp:269` — `spiHz = _x3Mode ? 10000000 : 40000000`)
- I2C on GPIO20/GPIO0 @ 400 kHz (`lib/hal/HalGPIO.h:21-23`): BQ27220 @ 0x55, DS3231 @ 0x68, QMI8658 @ 0x6B (unused)
- Battery assumed 1500–2500 mAh Li-Po

---

## 1. Deep-sleep current ceiling

**Datasheet targets** (ESP32-C3 datasheet, *Current Consumption in Low-Power Modes*):

| Mode | Typical I | Notes |
|---|---|---|
| Deep sleep, RTC timer only | **~5 µA** | CPU off, RF off, RTC_FAST mem off, RTC_SLOW_MEM retained |
| Deep sleep, RTC timer + GPIO wake | **~5 µA** | C3's GPIO deep-sleep wake uses the always-on digital island |
| Deep sleep + ULP | not available on C3 | C3 has no ULP coprocessor |
| Light sleep | 130–150 µA | CPU clock-gated, RAM retained |
| Hibernation (all RTC off, no wake but reset) | ~1 µA | No wall-clock retention |

**For the X3 topology this whole table is moot on battery.** `HalPowerManager::startDeepSleep` (`lib/hal/HalPowerManager.cpp:63-86`) drives **GPIO13 low, opening the battery-latch MOSFET and fully removing power from the MCU.** Comment at `HalPowerManager.cpp:72-75` is explicit: *"the MCU will be completely powered off during sleep, including RTC."* In this mode the ceiling is not `esp_deep_sleep` current but the **quiescent draw of whatever is still on the battery rail** — on X3 that is at least BQ27220 (typical 21–45 µA) + DS3231 (typical 1.5–110 µA depending on mode).

**Implication:** The MOSFET-off topology already beats any `esp_deep_sleep` configuration. The remaining off-state budget is **~20–50 µA**, dominated by BQ27220 + DS3231 + leakage through the MOSFET and display panel. This is a hardware-fixed ceiling; firmware cannot reduce it.

**CRITICAL caveat — timer wake:** Because the MOSFET kills the MCU entirely, `esp_sleep_enable_timer_wakeup()` **cannot wake us**. TRMNL's 15/30-min cycle must therefore use the **DS3231's alarm output wired into the power-button wake path**, or accept button-wake only. **This is the single biggest open hardware question for TRMNL on X3.** Needs probe of DS3231 INT/SQW pin routing on the X3 PCB. **Flagged: needs hardware investigation.**

**Second critical caveat:** `RTC_DATA_ATTR` **does NOT persist** across sleep on X3 (MCU fully powers off). All state that must survive sleep goes to NVS (`Preferences`).

---

## 2. Active-awake power budget

Typical ESP32-C3 current draw:
- WiFi TX (11n, MCS0, +20 dBm): ~335 mA peak
- WiFi TX (avg during connect burst): ~240 mA
- WiFi RX: ~85 mA
- CPU active, radio idle @ 160 MHz: ~22 mA
- CPU active, radio idle @ 80 MHz: ~16 mA

Per-phase estimate for a TRMNL cycle (PNG ~50–100 KB for 1bpp 792×528):

| Phase | Duration | Avg current | Charge (mC) |
|---|---|---|---|
| Boot + SDK init | 0.3 s | 60 mA | 18 |
| WiFi scan + associate (DHCP) | **2.5 s** | 100 mA | 250 |
| WiFi scan + associate (static IP, cached BSSID) | **0.6 s** | 100 mA | 60 |
| TLS handshake (fresh) | 1.0 s | 90 mA | 90 |
| TLS handshake (resumed) | 0.2 s | 90 mA | 18 |
| HTTP GET `/api/display` + JSON parse | 0.4 s | 80 mA | 32 |
| PNG download (80 KB @ ~200 KB/s) | 0.4 s | 80 mA | 32 |
| PNG decode (PNGdec, 80 KB) | **0.8 s** | 22 mA | 18 |
| E-ink full refresh (1.6 s BUSY) | 1.6 s | 25 mA | 40 |
| E-ink power-down + MCU sleep prep | 0.3 s | 20 mA | 6 |
| **Total — cold path, DHCP, fresh TLS** | **~7.5 s** | — | **~486 mC = 135 µAh** |
| **Total — cached BSSID, resumed TLS** | **~4.3 s** | — | **~224 mC = 62 µAh** |

**80%-impact opportunities (ranked):**
1. **Cut WiFi connect time** via BSSID+channel+IP caching in NVS — saves ~190 mC/cycle ≈ 53 µAh/cycle. Biggest single lever.
2. **Skip the whole cycle on cache-hit** (same `filename` as last) — saves ~300 mC.
3. **TLS session resumption across deep-sleep** — saves ~0.8 s × 90 mA = 72 mC/cycle.
4. Decode is 0.8 s at 22 mA = 18 mC — not worth optimizing until the above are done.

---

## 3. WiFi-specific optimizations

**Static IP vs DHCP:** DHCP is 4 broadcast round-trips, 400–1200 ms. Configure `WiFi.config(ip, gw, sn, dns)` before `WiFi.begin()`. Saves ~0.5 s × 100 mA ≈ 14 µAh/cycle.

**BSSID+channel caching:** `WiFi.begin(ssid, pass, channel, bssid)` skips scan + channel probe. Persist to NVS (RTC_DATA_ATTR doesn't work — §1). Refresh cache on any connect failure.

**`esp_wifi_set_ps(WIFI_PS_NONE)`:** For short-lived awake sessions, PS-mode hurts throughput. `src/network/OtaUpdater.cpp:233` gets this right — do same in TRMNL path.

**TLS session resumption:** ESP-IDF mbedtls supports session tickets. Persist ~200–600 bytes to NVS keyed by hostname.

**Cert validation:** Current code uses `setInsecure()` (`src/network/HttpDownloader.cpp:60`). Acceptable for `trmnl.fradski.me` (user's infra). If cert pinning added, use `esp_crt_bundle_attach` like OTA (`src/network/OtaUpdater.cpp:223`).

**HTTP/2 vs 1.1:** ESP-IDF HTTP client is 1.1 only. H/2 would need nghttp2 — too big. 1.1 is fine for one-request-per-cycle.

---

## 4. CPU frequency

WiFi air-time dominates every phase that matters. At 160 vs 80 MHz:
- Radio-bound phases: no wall-clock difference (bottleneck is 2.4 GHz throughput)
- PNG decode: 1.6× faster at 160 MHz. Saves 0.3 s × 22 mA = 6.6 mC
- CPU current delta: +6 mA × 0.8 s = 4.8 mC cost

**Net at 160 MHz:** saves 1.8 mC/cycle. Marginal. Keep at 160. CrossPoint's `HalPowerManager` dynamically downshifts to 10 MHz in idle — not applicable to TRMNL which is never idle while awake.

---

## 5. E-ink specifics

`EInkDisplay::deepSleep()` (`open-x4-sdk/libs/display/EInkDisplay/src/EInkDisplay.cpp:1190-1214`) runs panel power-down + SSD1677 `0x10 0x01` deep-sleep. **Saves 100–500 µA** (panel standby without it) vs **<1 µA** (panel in deep-sleep mode). Over 15 min at 200 µA = 50 mAh/day — huge leak if skipped. **Always call before MOSFET-off.** CrossPoint already does: `src/main.cpp:188`.

**Partial vs full refresh:**
- Full: ~1.6 s BUSY × 25 mA = 40 mC
- Partial/fast: ~0.6 s BUSY × 25 mA = 15 mC

**Policy:** Use `FAST_REFRESH` default. Schedule `FULL_REFRESH` once/hour or every N fast refreshes to clear ghosting. Saves 25 mC × 75% of cycles ≈ 5 µAh/cycle average.

---

## 6. Image pipeline

**Cache-hit fast path:** Same `filename` from `/api/display` as last render → skip download + decode + refresh.
- Worst-case cycle: 7.5 s = 135 µAh
- Cache-hit cycle: ~3 s × 90 mA = 270 mC = **75 µAh**
- **Savings: 60 µAh/cycle** when dashboard unchanged. Over 96 cycles/day = **5.8 mAh/day saved**.

Even better: use `If-None-Match` / `304 Not Modified` with ETag (persist in NVS). 304 is ~200 bytes vs 80 KB, saving another ~100 ms of RX. **Implement both**: ETag first (standard), filename-compare as fallback.

**PNGdec RAM cost:** `PNG_MAX_BUFFERED_PIXELS=16416` × 4 bytes = ~65 KB scanline buffer. For X3 (792px), actual need is 792×4 = 3168 B. **Lower to `PNG_MAX_BUFFERED_PIXELS=800` to free ~60 KB DRAM.** See `platformio.ini:36`.

---

## 7. Peripherals audit

- **BQ27220:** read once per wake, cached. Correct.
- **DS3231:** not currently read. For TRMNL: read once at wake for wall-clock. Pattern in `HalGPIO.cpp:100-112` is the template.
- **QMI8658:** never initialized. Leave disabled.
- **SPI:** add `SPI.end()` before sleep defensively.
- **I2C:** `Wire.end() + pinMode(20, INPUT); pinMode(0, INPUT);` (pattern at `HalGPIO.cpp:109-111`).
- **UART/USB-CDC:** in production env, keep `ENABLE_SERIAL_LOG` off to avoid USB stack init at every boot. **Note `gh_release` currently enables serial log — flag for review.**

---

## 8. GPIO holds — needs multimeter test

**CrossPoint (X4):** drives GPIO13 **LOW** → opens MOSFET → MCU fully off.

**For X3:** BQ27220 has its own always-on rail (required for monotonic SoC across sleep). This means two rails:
1. Always-on (VBAT → BQ27220, DS3231 VBAT, possibly display VIN standby)
2. Switched (VBAT → MOSFET → MCU VDD + display main + other SPI peripherals)

**Recommendation: keep GPIO13 LOW pattern.** Do NOT follow "X4 TRMNL port holds HIGH" — BQ27220 independence makes MOSFET-off correct on X3.

**Verify on hardware:**
- [ ] Probe GPIO13 asleep: ~0 V
- [ ] Probe VDD_MCU asleep: ~0 V
- [ ] Probe VBAT_BQ27220 asleep: ~3.7 V
- [ ] Total battery current asleep: target <50 µA

---

## 9. RTC memory usage

RTC_SLOW_MEM **not retained** across sleep on X3 (MOSFET-off). Use NVS (`Preferences`):

| Key | Size | Purpose | Saves/cycle |
|---|---|---|---|
| `trmnl.bssid` | 6 B | Skip WiFi scan | ~28 µAh |
| `trmnl.channel` | 1 B | Skip channel probe | (incl. above) |
| `trmnl.ip` | 4 B | Static IP | ~14 µAh |
| `trmnl.gw/sn/dns` | 12 B | Gateway/netmask/DNS | (incl. above) |
| `trmnl.etag` | ~40 B | HTTP 304 cache key | ~3–9 µAh |
| `trmnl.filename` | ~64 B | Cache key fallback | same |
| `trmnl.tls_session` | ~300 B | TLS resumption | ~20 µAh |
| `trmnl.wake_count` | 4 B | Forces full refresh every N | — |
| `trmnl.last_full_refresh_ts` | 4 B | Hourly full-refresh policy | — |

Total NVS: <500 B. Write on successful cycle, read on every wake.

---

## 10. Refresh rate policy

**Assumptions:** 2000 mAh battery, off-state 30 µA (720 µAh/day baseline), per-cycle 80 µAh avg.

| Interval | Wakes/day | µAh/day | Months |
|---|---|---|---|
| 15 min | 96 | 8400 | ~7.9 |
| 30 min | 48 | 4560 | ~14.4 |
| 15 min (optimized 40 µAh/cycle) | 96 | 4560 | ~14.4 |
| 30 min (optimized 40 µAh/cycle) | 48 | 2640 | ~25 |

**Recommended default: 30 min.** Expose 15/30/60/manual as user settings.

**Aggressive target:** <40 µAh/cycle + 30-min interval + 2500 mAh battery = **~3 years**.

---

## 11. Wake reason handling

- `PowerButton` → full cycle (user wants latest).
- `AfterUSBPower` → immediate sleep (avoids rapid boot cycles on USB plug).
- `AfterFlash`/`Other` → normal cycle.

**Optimization — button-wake cache shortcut:** If user presses button within 60 s of prior refresh, skip WiFi + re-render cached PNG. Saves ~40 µAh. Implement via NVS `trmnl.last_refresh_ts`.

---

## 12. Audit checklist for PRs

**Sleep prep:**
- [ ] `display.deepSleep()` before `powerManager.startDeepSleep(gpio)`
- [ ] `SPI.end()`, `Wire.end()`, SD card `end()` before sleep
- [ ] I2C pins released: `pinMode(20, INPUT); pinMode(0, INPUT);`
- [ ] `WiFi.disconnect(true, true); WiFi.mode(WIFI_OFF);` before sleep
- [ ] GPIO13 drive+hold matches `HalPowerManager.cpp:72-77` for X3 (LOW)

**Awake path:**
- [ ] No `delay(N > 50 ms)` outside sleep/idle
- [ ] No blocking `while()` without timeout
- [ ] `esp_wifi_set_ps(WIFI_PS_NONE)` before any HTTP burst
- [ ] `WiFi.config()` called before `WiFi.begin()` when static IP cached
- [ ] `WiFi.begin(ssid, pass, channel, bssid)` when cache valid
- [ ] No WiFi scan on normal wake path (config-mode only)
- [ ] HTTP client released (`http.end()`) on every exit path

**Memory:**
- [ ] PNGdec buffer size matches panel width, not generic 2048px macro
- [ ] No `std::function` in refresh hot path
- [ ] `ESP.getFreeHeap()` returns to baseline pre-sleep

**Caching:**
- [ ] ETag/filename persisted to NVS before sleep
- [ ] BSSID+channel+static IP persisted after successful connect
- [ ] TLS session persisted after successful handshake
- [ ] Cache-hit path skips download AND decode AND refresh (all three)
- [ ] Cache invalidation on connect failure

**Diagnostics:**
- [ ] Wake reason logged
- [ ] Wake-count incremented in NVS
- [ ] Battery % read once post-wake, not polled
- [ ] `ENABLE_SERIAL_LOG` off in battery prod env

**Refresh policy:**
- [ ] `FAST_REFRESH` default; `FULL_REFRESH` gated by hourly/N-count policy
- [ ] Refresh interval configurable; default 30 min

---

## Top 10 recommendations (ranked by µAh/day impact)

Assuming 30-min cycle (48 wakes/day) and 2000 mAh battery, baseline ~8400 µAh/day.

| # | Action | µAh/cycle | µAh/day | % budget |
|---|---|---:|---:|---:|
| 1 | **BSSID+channel cache in NVS** | 28 | 1344 | 16% |
| 2 | **Filename/ETag cache-hit fast path** | 60 × 50% cycles | 1440 | 17% |
| 3 | **Static IP via NVS** | 14 | 672 | 8% |
| 4 | **TLS session resumption (NVS)** | 20 | 960 | 11% |
| 5 | **WIFI_PS_NONE before GET; WIFI_OFF before sleep** | 5 | 240 | 3% |
| 6 | **30-min default vs 15-min** | — | 4200 | 50% vs 15-min |
| 7 | **FAST_REFRESH default; full every hour** | 5 | 240 | 3% |
| 8 | **`PNG_MAX_BUFFERED_PIXELS=800`** (free ~60 KB DRAM) | ~2 | 96 | 1% |
| 9 | **`SPI.end()`+`Wire.end()` before sleep** (defensive) | 0 | 0 | — |
| 10 | **Button-wake cache shortcut (<60 s)** | 40 × manual presses | ~200 | 2% |

**Combined 1+2+3+4+5+7:** ~80% of cycle cost. 8400 → ~2600 µAh/day. **~8 months → ~2.5 years** on 2000 mAh, 30-min cycle.

**Prerequisites not solved by firmware:**
- **§1/§8 DS3231 alarm routing** — required for battery timer-wake. Without it, X3 is button-wake only.
- **MOSFET-off current measurement** — target <50 µA total. Multimeter required.
