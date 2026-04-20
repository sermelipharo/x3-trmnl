#include "TrmnlActivity.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <InputManager.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include <vector>

#include "../../WifiCredentialStore.h"
#include "../../fontIds.h"
#include "../../trmnl/TrmnlApi.h"
#include "../../trmnl/TrmnlConfig.h"
#include "../../trmnl/TrmnlImage.h"
#include "../../trmnl/TrmnlOta.h"
#include "../../trmnl/TrmnlState.h"
#include <GfxRenderer.h>
#include <HalDisplay.h>

namespace {
// Deep sleep with both timer and power-button wake. Keeping the MCU's
// RTC running costs ~5–10 µA extra quiescent but avoids needing a
// separate RTC chip (DS3231) just to schedule the next refresh.
[[noreturn]] void startTimerSleep(uint32_t refreshSeconds) {
  // Keep GPIO13 HIGH through deep sleep so the battery-latch MOSFET stays
  // closed and the MCU (and its RTC) stays powered. Without the hold the
  // timer wake never fires on battery because the MCU dies the moment we
  // enter sleep.
  //
  // Clear any prior lock first so gpio_set_level takes effect.
  gpio_hold_dis(GPIO_NUM_13);
  gpio_deep_sleep_hold_dis();
  gpio_set_direction(GPIO_NUM_13, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_13, 1);
  gpio_hold_en(GPIO_NUM_13);
  gpio_deep_sleep_hold_en();

  // Release I2C / SPI so they don't leak current via pull-ups during sleep.
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  SPI.end();

  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN,
                                    ESP_GPIO_WAKEUP_GPIO_LOW);
  if (refreshSeconds > 0) {
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(refreshSeconds) * 1000000ULL);
  }
  LOG_INF("TRMNL", "Deep sleep (timer=%us, power-button wake, GPIO13 released)",
          (unsigned)refreshSeconds);
  esp_deep_sleep_start();
  while (true) {
  }  // [[noreturn]]
}
}  // namespace

void TrmnlActivity::onEnter() {
  Activity::onEnter();
  TRMNL_STATE.load();
  TRMNL_STATE.incrementWakeCount();
  LOG_INF("TRMNL", "Wake #%u start", TRMNL_STATE.getWakeCount());
  runCycleAndSleep();
}

bool TrmnlActivity::connectWifi() {
  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    LOG_ERR("TRMNL", "No WiFi credentials stored");
    return false;
  }

  // Disable WiFi modem power-save for the short awake session — PS mode
  // hurts throughput and adds latency, and we're awake for a few seconds.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Build try-order: last-connected first (fast path when the user hasn't
  // changed networks), then the rest in insertion order. Covers the case
  // where a stale bootstrap credential sits before a newer portal-saved one.
  std::vector<const WifiCredential*> tryOrder;
  tryOrder.reserve(creds.size());
  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    if (const WifiCredential* c = WIFI_STORE.findCredential(lastSsid)) {
      tryOrder.push_back(c);
    }
  }
  for (const auto& c : creds) {
    if (!tryOrder.empty() && tryOrder.front() == &c) continue;
    tryOrder.push_back(&c);
  }

  // Retry within a single wake so a wrong password fails fast (3 × ~15 s =
  // ~45 s max) instead of burning 3 wake cycles waiting for auto-recovery.
  constexpr int kMaxAttemptsPerCred = 3;
  for (const WifiCredential* cred : tryOrder) {
    for (int attempt = 1; attempt <= kMaxAttemptsPerCred; attempt++) {
      LOG_INF("TRMNL", "Connecting to %s (attempt %d/%d)", cred->ssid.c_str(),
              attempt, kMaxAttemptsPerCred);
      WiFi.disconnect(false, true);
      delay(100);
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      const uint32_t deadline = millis() + TRMNL_WIFI_CONNECT_TIMEOUT_MS;
      while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(100);
      }
      if (WiFi.status() == WL_CONNECTED) {
        WIFI_STORE.setLastConnectedSsid(cred->ssid);
        WIFI_STORE.saveToFile();
        TRMNL_STATE.setWifiFailCount(0);
        LOG_INF("TRMNL", "WiFi OK — ssid=%s ip=%s rssi=%d", cred->ssid.c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
      }
      LOG_ERR("TRMNL", "WiFi attempt %d failed (ssid=%s, status=%d)", attempt,
              cred->ssid.c_str(), (int)WiFi.status());
    }
  }

  LOG_ERR("TRMNL", "All WiFi credentials exhausted after retries");
  return false;
}

[[noreturn]] void TrmnlActivity::runCycleAndSleep() {
  const bool wifiOk = connectWifi();
  TrmnlDisplayResponse resp;  // scope extends to sleep-scheduling below

  if (wifiOk) {
    const uint16_t battMV = powerManager.getBatteryVoltageMV();
    const float battVoltage = (battMV > 0) ? (battMV / 1000.0f) : 4.0f;
    const int rssi = WiFi.RSSI();
    LOG_INF("TRMNL", "Battery: %u mV (%u%%)", (unsigned)battMV, (unsigned)powerManager.getBatteryPercentage());

    // Provision on first wake (or after a portal-driven server switch):
    // no api_key in NVS → GET /api/setup. On success we persist the
    // returned api_key + friendly_id, render the server's splash, and
    // sleep. The next wake enters the normal display cycle.
    if (TRMNL_STATE.getApiKey().empty()) {
      LOG_INF("TRMNL", "No api_key in NVS — running /api/setup provisioning");
      TrmnlSetupResponse setupResp;
      bool setupOk = TrmnlApi::setup(setupResp);
      if (setupOk && setupResp.status == 200 && !setupResp.api_key.empty()) {
        TRMNL_STATE.setApiKey(setupResp.api_key);
        TRMNL_STATE.setFriendlyId(setupResp.friendly_id);
        TRMNL_STATE.setSetupFailCount(0);
        TRMNL_STATE.save();
        LOG_INF("TRMNL", "Provisioned — friendly_id=%s", setupResp.friendly_id.c_str());
        if (!setupResp.image_url.empty() &&
            TrmnlImage::downloadAndRender(setupResp.image_url, /*fullRefresh=*/true)) {
          // Only cache the filename if the image actually rendered; otherwise
          // the server could later reuse this key and our cache-hit path
          // would skip the first real redraw.
          TRMNL_STATE.setLastFilename(setupResp.filename);
        } else {
          renderSetupSplash(setupResp.friendly_id);
        }
        resp.refresh_rate = TRMNL_DEFAULT_REFRESH_SECONDS;
      } else {
        // Exponential backoff keeps a misconfigured server (unknown MAC,
        // wrong URL, bad DNS) from eating the whole battery. Each failed
        // wake doubles the sleep interval up to ~24 h.
        const uint8_t fails = TRMNL_STATE.getSetupFailCount() + 1;
        TRMNL_STATE.setSetupFailCount(fails);
        LOG_ERR("TRMNL", "Setup failed (ok=%d status=%d, fails=%u) — backing off",
                (int)setupOk, (int)setupResp.status, (unsigned)fails);
        renderSetupFailure();
        // 1800 s <<= fails capped at 86400 s (24 h). fails=1 → 1 h, fails=2
        // → 2 h, fails=5 → 16 h, fails=6+ → 24 h.
        uint32_t backoff = static_cast<uint32_t>(TRMNL_DEFAULT_REFRESH_SECONDS) *
                           (1u << ((fails > 6) ? 6 : fails));
        if (backoff > 86400u) backoff = 86400u;
        resp.refresh_rate = backoff;
      }
    } else if (TrmnlApi::fetchDisplay(resp, battVoltage, rssi)) {
      // Server-initiated unprovision. BYOD contract: wipe api_key + WiFi
      // creds, reboot into captive portal. The next boot re-runs the full
      // provisioning flow from scratch.
      if (resp.reset_firmware) {
        LOG_INF("TRMNL", "reset_firmware=true — wiping creds and rebooting");
        // Persist the "go to portal + api_key cleared" state BEFORE wiping
        // WiFi. If power dies between the two writes, next boot still has
        // portalRequested=true and an empty api_key, so it enters the
        // captive portal instead of trying to authenticate with stale data.
        TRMNL_STATE.setApiKey("");
        TRMNL_STATE.setFriendlyId("");
        TRMNL_STATE.setLastFilename("");
        TRMNL_STATE.setLastOtaUrl("");
        TRMNL_STATE.setLastRefreshRate(0);
        TRMNL_STATE.setWifiFailCount(0);
        TRMNL_STATE.setSetupFailCount(0);
        TRMNL_STATE.setPortalRequested(true);
        TRMNL_STATE.save();
        WIFI_STORE.clearAll();
        WIFI_STORE.saveToFile();
        delay(500);
        ESP.restart();
      }

      // OTA: spec piggybacks firmware updates on /api/display. The BYOD
      // response is not signed, so we gate auto-apply behind TRMNL_ENABLE_OTA
      // at build time. We also skip when the server repeats a URL we already
      // flashed — prevents a "server always returns update_firmware=true"
      // loop from reflashing every wake and draining the battery.
      if (resp.update_firmware && !resp.firmware_url.empty()) {
#ifdef TRMNL_DISABLE_OTA
        LOG_INF("TRMNL",
                "update_firmware=true ignored — OTA disabled at build time (undefine TRMNL_DISABLE_OTA to re-enable)");
#else
        // URL dedup prevents a misbehaving server that always flags
        // update_firmware=true from reflashing every wake (= battery
        // death in days). When the firmware_url matches the most
        // recently applied one we skip apply and continue normally.
        if (resp.firmware_url == TRMNL_STATE.getLastOtaUrl()) {
          LOG_INF("TRMNL", "update_firmware=true but URL matches last applied — skipping");
        } else {
          LOG_INF("TRMNL", "update_firmware=true — applying OTA from %s",
                  resp.firmware_url.c_str());
          if (TrmnlOta::applyOtaUpdate(resp.firmware_url)) {
            TRMNL_STATE.setLastOtaUrl(resp.firmware_url);
            TRMNL_STATE.save();
            LOG_INF("TRMNL", "OTA success — rebooting");
            delay(500);
            ESP.restart();
          }
          LOG_ERR("TRMNL", "OTA failed — continuing normal cycle");
        }
#endif
      }

      // status != 0 means the server rejected the request. 202 = device
      // known but not yet linked to a playlist (user needs to finish setup
      // in the server UI). 500 = access token invalid or other server
      // error. Show a status splash and back off — don't render stale
      // image data we didn't get.
      // Spec convention is status=0 for OK; some BYOS implementations use
      // HTTP-style 200 in the body. Treat both as success. 202 = not yet
      // linked, anything else = server error.
      const bool statusOk = (resp.status == 0 || resp.status == 200);
      if (!statusOk) {
        LOG_ERR("TRMNL", "Display cycle aborted — server status=%d", (int)resp.status);
        if (resp.status == 202) {
          renderPendingLink();
        } else {
          renderServerError(resp.status);
        }
        // status=500 typically means the stored api_key is invalid, a
        // device-side state the server already knows — logging it every
        // wake just doubles HTTPS traffic. Intentionally no postLog here.
        //
        // Keep server-provided refresh_rate if any, else default.
        if (resp.refresh_rate == 0) resp.refresh_rate = TRMNL_DEFAULT_REFRESH_SECONDS;
      } else {
#ifdef TRMNL_FORCE_CACHE_MISS
        const bool cacheHit = false;
        LOG_INF("TRMNL", "TRMNL_FORCE_CACHE_MISS — treating as cache miss");
#else
        const bool cacheHit = (!resp.filename.empty() && resp.filename == TRMNL_STATE.getLastFilename());
#endif
        if (cacheHit) {
          LOG_INF("TRMNL", "Cache HIT on filename='%s' — skipping download/decode/refresh",
                  resp.filename.c_str());
        } else {
          LOG_INF("TRMNL", "Cache MISS (server='%s', last='%s') — rendering new image",
                  resp.filename.c_str(), TRMNL_STATE.getLastFilename().c_str());
          // FULL_REFRESH every 16 wakes to clear ghosting.
          const bool fullRefresh = (TRMNL_STATE.getWakeCount() % 16) == 1;
          if (TrmnlImage::downloadAndRender(resp.image_url, fullRefresh)) {
            TRMNL_STATE.setLastFilename(resp.filename);
          } else {
            LOG_ERR("TRMNL", "Render failed — keeping previous cache key");
          }
        }
        // Remember the cadence the server asked for; reported back in the
        // Refresh-Rate header on subsequent /api/display calls.
        if (resp.refresh_rate > 0) {
          TRMNL_STATE.setLastRefreshRate(resp.refresh_rate);
        }
      }
    }
  } else {
    // Bump consecutive-wake fail counter in NVS. A single wake of 3 retry
    // attempts is NOT enough to wipe — a router reboot or transient outage
    // should be tolerated. Wipe creds only after WIFI_FAIL_THRESHOLD
    // consecutive wakes fail (≈2.5 h at 15-min cycle, ≈5 h at 30-min).
    const uint8_t fails = TRMNL_STATE.getWifiFailCount() + 1;
    TRMNL_STATE.setWifiFailCount(fails);
    LOG_ERR("TRMNL", "WiFi failed all retries in this wake (consecutive fails=%u/%u)",
            (unsigned)fails, (unsigned)TrmnlState::WIFI_FAIL_THRESHOLD);

    if (fails < TrmnlState::WIFI_FAIL_THRESHOLD) {
      TRMNL_STATE.save();
      // Skip API cycle, but keep timer-wake scheduling — device will retry
      // next wake. Fall through to the sleep scheduling below.
    } else {
      LOG_ERR("TRMNL", "WiFi failed across %u wakes — wiping creds and rebooting into captive portal",
              (unsigned)fails);
    renderer.clearScreen();
    const int h = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2 - 40, "WiFi failed", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2,
                              "Starting setup in a moment...", true);
    renderer.drawCenteredText(SMALL_FONT_ID, h / 2 + 30,
                              "Join TRMNL-XXXXXX WiFi to reconfigure.");
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);

      WIFI_STORE.clearAll();
      WIFI_STORE.saveToFile();
      TRMNL_STATE.setWifiFailCount(0);
      TRMNL_STATE.setPortalRequested(true);  // suppress build-flag bootstrap next boot
      TRMNL_STATE.save();
      delay(2000);
      ESP.restart();  // portal comes up on next boot (no creds)
    }
  }

  // Persist state even on failure paths so wakeCount reflects reality.
  TRMNL_STATE.save();

  // Cut WiFi before deep sleep so the radio is fully off on entry.
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  // Developer-mode mitigation for USB boot-loop + serial-log window: MOSFET-
  // off deep sleep is immediately undone by USB VBUS, causing rapid
  // wake→cycle→sleep→wake while plugged in. Also, the port enumerate/drop
  // races with host-side serial monitors. Holding 30 s on dev builds gives
  // readable logs AND accepts CMD:WIPE/CMD:REFRESH via serial. Production
  // builds skip this and go straight to sleep.
  //
  // Intentionally NOT gated on gpio.isUsbConnected() — BQ27220 Current()
  // reports 0 when the battery is topped up even with USB plugged in.
#ifdef ENABLE_SERIAL_LOG
  // Hold only when a USB host is actually attached — HWCDC.operator bool()
  // goes true after CDC enumerate, false when unplugged. On battery this
  // skips the hold entirely so we don't burn ~700 µAh/wake idling.
  if (logSerial) {
    LOG_INF("TRMNL", "USB host detected — holding 30 s (for serial + CMD)");
    const uint32_t holdDeadline = millis() + 30000;
    while (millis() < holdDeadline) {
      if (logSerial.available()) {
        String line = logSerial.readStringUntil('\n');
        line.trim();
        if (line == "CMD:WIPE") {
          LOG_INF("TRMNL", "CMD:WIPE — wiping creds + portalRequested, restart");
          WIFI_STORE.clearAll();
          WIFI_STORE.saveToFile();
          TRMNL_STATE.setPortalRequested(true);
          TRMNL_STATE.save();
          delay(500);
          ESP.restart();
        } else if (line == "CMD:REFRESH") {
          LOG_INF("TRMNL", "CMD:REFRESH — clearing filename cache, restart");
          TRMNL_STATE.setLastFilename("");
          TRMNL_STATE.save();
          delay(500);
          ESP.restart();
        } else if (line == "CMD:STATUS") {
          LOG_INF("TRMNL", "creds=%u portalReq=%d wakeCount=%u fails=%u lastFile=%s",
                  (unsigned)WIFI_STORE.getCredentials().size(),
                  (int)TRMNL_STATE.isPortalRequested(),
                  TRMNL_STATE.getWakeCount(),
                  TRMNL_STATE.getWifiFailCount(),
                  TRMNL_STATE.getLastFilename().c_str());
        }
      }
      delay(50);
    }
  }
#endif

  // Schedule the next wake. Server's refresh_rate (seconds) wins; fall back
  // to TRMNL_DEFAULT_REFRESH_SECONDS if we never got a successful fetch.
  const uint32_t refreshSec = (wifiOk && resp.refresh_rate > 0) ? resp.refresh_rate
                                                                : TRMNL_DEFAULT_REFRESH_SECONDS;
  LOG_INF("TRMNL", "Cycle done → deep sleep for %u s", (unsigned)refreshSec);
  display.deepSleep();
  startTimerSleep(refreshSec);
}

void TrmnlActivity::renderSetupSplash(const std::string& friendlyId) {
  renderer.clearScreen();
  const int h = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2 - 40, "TRMNL ready", true,
                            EpdFontFamily::BOLD);
  if (!friendlyId.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2,
                              (std::string("Link ID: ") + friendlyId).c_str(), true);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2,
                              "Link this device in your server UI", true);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, h / 2 + 30,
                            "Refresh will start after the next wake.");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void TrmnlActivity::renderSetupFailure() {
  renderer.clearScreen();
  const int h = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2 - 40, "Setup failed", true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2,
                            "Server unreachable or device not registered.", true);
  renderer.drawCenteredText(SMALL_FONT_ID, h / 2 + 30,
                            "Will retry on next wake.");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void TrmnlActivity::renderPendingLink() {
  renderer.clearScreen();
  const int h = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2 - 40, "Waiting for link", true,
                            EpdFontFamily::BOLD);
  const std::string& friendly = TRMNL_STATE.getFriendlyId();
  if (!friendly.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, h / 2,
                              (std::string("Link ID: ") + friendly).c_str(), true);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, h / 2 + 30,
                            "Finish setup in the server UI.");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void TrmnlActivity::renderServerError(int32_t status) {
  renderer.clearScreen();
  const int h = renderer.getScreenHeight();
  char line[64];
  snprintf(line, sizeof(line), "Server error (status %d)", (int)status);
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2 - 40, line, true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, h / 2,
                            "Check access token and server logs.", true);
  renderer.drawCenteredText(SMALL_FONT_ID, h / 2 + 30,
                            "Will retry on next wake.");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
