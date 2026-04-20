#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "fontIds.h"
#include "trmnl/TrmnlState.h"
#include "util/ButtonNavigator.h"

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
FontCacheManager fontCacheManager(renderer.getFontMap());

// Minimal font set — just UI and small for now. We'll add more when the TRMNL
// render path needs them.
EpdFont uiRegularFont(&ubuntu_12_regular);
EpdFontFamily uiFontFamily(&uiRegularFont);

EpdFont smallFont(&opendyslexic_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  renderer.insertFont(UI_12_FONT_ID, uiFontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  // Release any lingering GPIO13 hold from CrossPoint-era sleeps. Previous
  // firmware called gpio_hold_en(13) + gpio_deep_sleep_hold_en(); those
  // persist across deep sleep AND re-boots until explicitly disabled.
  // Without this, our timer-wake path still MOSFET-offs the MCU and timer
  // wake never fires.
  gpio_hold_dis(GPIO_NUM_13);
  gpio_deep_sleep_hold_dis();

  HalSystem::begin();
  gpio.begin();
  powerManager.begin();

#ifdef ENABLE_SERIAL_LOG
  // Dev builds: always start Serial. Gating on gpio.isUsbConnected() fails
  // when the BQ27220 I2C read races early boot, which silently suppresses
  // all serial logs. For battery-mode USB-CDC is a no-op anyway.
  Serial.begin(115200);
  const unsigned long serialStart = millis();
  while (!Serial && (millis() - serialStart) < 500) {
    delay(10);
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  I18N.loadSettings();
  WIFI_STORE.loadFromFile();
  TRMNL_STATE.load();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

#if defined(TRMNL_BOOTSTRAP_WIFI_SSID) && defined(TRMNL_BOOTSTRAP_WIFI_PASSWORD)
  // One-shot WiFi bootstrap via build flags. Runs when the store is empty
  // AND we haven't explicitly been told to go to portal (which happens after
  // a failed-WiFi auto-wipe — re-seeding the same bad creds there would
  // loop us straight back into the failure).
  if (WIFI_STORE.getCredentials().empty() && !TRMNL_STATE.isPortalRequested()) {
    LOG_INF("MAIN", "Bootstrapping WiFi credentials from build flags (SSID=%s)",
            TRMNL_BOOTSTRAP_WIFI_SSID);
    WIFI_STORE.addCredential(TRMNL_BOOTSTRAP_WIFI_SSID, TRMNL_BOOTSTRAP_WIFI_PASSWORD);
    WIFI_STORE.saveToFile();
  }
#endif

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Note: CrossPoint originally slept here to avoid rapid boot cycles
      // when a dead battery gets plugged in. For TRMNL we want the cycle to
      // run on USB power too — it's the common dev path and the natural
      // behavior once we wire timer-wake (task #25). Fall through.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power — running normal cycle");
      break;
    case HalGPIO::WakeupReason::AfterFlash:
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_INF("MAIN", "Starting x3-trmnl");

  setupDisplayAndFonts();

#ifdef TRMNL_BUTTON_DIAG
  // Diagnostic: log every button press to serial and to the e-ink panel so
  // we can map physical buttons to the logical index returned by
  // HalGPIO/InputManager. Runs forever; reset to exit.
  {
    LOG_INF("DIAG", "BUTTON DIAG — press each button in turn");
    renderer.clearScreen();
    const int h = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, 40, "BUTTON DIAG", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, 70,
                              "Press each button. Watch serial + the list below.");
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);

    int row = 120;
    uint32_t seq = 0;
    const char* names[7] = {"BACK(0)", "CONFIRM(1)", "LEFT(2)", "RIGHT(3)",
                            "UP(4)", "DOWN(5)", "POWER(6)"};
    while (true) {
      gpio.update();
      for (uint8_t i = 0; i <= HalGPIO::BTN_POWER; i++) {
        if (gpio.wasPressed(i)) {
          seq++;
          LOG_INF("DIAG", "#%u pressed: idx=%u name=%s", (unsigned)seq, (unsigned)i, names[i]);
          char line[64];
          snprintf(line, sizeof(line), "#%u  %s", (unsigned)seq, names[i]);
          renderer.drawText(UI_12_FONT_ID, 40, row, line, true);
          row += 20;
          if (row > h - 40) {
            renderer.clearScreen();
            renderer.drawCenteredText(UI_12_FONT_ID, 40, "BUTTON DIAG",
                                      true, EpdFontFamily::BOLD);
            row = 120;
          }
          renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        }
      }
      delay(30);
    }
  }
#endif

  // Deliberately skip BootActivity: the e-ink panel retains its last
  // displayed image across sleep, so on cache-hit the dashboard is still
  // on screen. Drawing a boot logo would overwrite it, and TrmnlActivity's
  // cache-hit path wouldn't redraw. Go straight to the target activity.
  APP_STATE.loadFromFile();

  // If we have no saved WiFi credentials, drop into captive portal for
  // first-time setup instead of running the TRMNL cycle. Holding BACK
  // during wake also forces portal mode; we poll for ~1.5 s so the user
  // can hold BACK, then tap power, and have the button still down when
  // we check.
  const bool noCreds = WIFI_STORE.getCredentials().empty();
  // Hold BACK while powering on to force the captive portal. We poll for
  // ~5 s after the initial gpio.begin() because mappedInputManager state
  // is only valid after at least one update(), and the user typically
  // taps power first then the button settles.
  bool forceConfig = false;
  {
    const uint32_t deadline = millis() + 5000;
    while (millis() < deadline) {
      gpio.update();
      if (mappedInputManager.isPressed(MappedInputManager::Button::Back)) {
        forceConfig = true;
        break;
      }
      delay(30);
    }
  }
  const bool portalReq = TRMNL_STATE.isPortalRequested();
#ifdef TRMNL_FORCE_CAPTIVE_PORTAL
  const bool devForce = true;
#else
  const bool devForce = false;
#endif
  if (noCreds || forceConfig || portalReq || devForce) {
    LOG_INF("MAIN", "Entering captive portal (noCreds=%d forceConfig=%d portalReq=%d devForce=%d)",
            noCreds, forceConfig, portalReq, devForce);
    activityManager.goToCaptivePortal();
  } else {
    activityManager.goToTrmnl();
  }

  // Ensure we're not still holding the power button before leaving setup
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

void loop() {
  gpio.update();

  // Dev-mode serial commands. Runs between activity frames, so it's active
  // whenever we're in an activity's loop() (TrmnlActivity's USB-hold phase,
  // captive portal, sleep screen, etc).
  //   CMD:WIPE    — clear WiFi creds + set portalRequested, restart.
  //   CMD:STATUS  — dump current creds count + portalReq + wake count.
#ifdef ENABLE_SERIAL_LOG
  // `Serial` is remapped to MySerialImpl (write-only) by Logging.h — use the
  // underlying logSerial (real HWCDC) to read input.
  if (logSerial.available()) {
    String line = logSerial.readStringUntil('\n');
    line.trim();
    if (line == "CMD:WIPE") {
      LOG_INF("MAIN", "CMD:WIPE — wiping creds and restarting into portal");
      WIFI_STORE.clearAll();
      WIFI_STORE.saveToFile();
      TRMNL_STATE.setPortalRequested(true);
      TRMNL_STATE.save();
      delay(500);
      ESP.restart();
    } else if (line == "CMD:STATUS") {
      LOG_INF("MAIN", "creds=%u portalReq=%d wakeCount=%u fails=%u lastFile=%s",
              (unsigned)WIFI_STORE.getCredentials().size(),
              (int)TRMNL_STATE.isPortalRequested(),
              TRMNL_STATE.getWakeCount(),
              TRMNL_STATE.getWifiFailCount(),
              TRMNL_STATE.getLastFilename().c_str());
    } else if (line == "CMD:REFRESH") {
      LOG_INF("MAIN", "CMD:REFRESH — clearing cached filename, restarting");
      TRMNL_STATE.setLastFilename("");
      TRMNL_STATE.save();
      delay(500);
      ESP.restart();
    } else if (line.length() > 0) {
      LOG_INF("MAIN", "unknown cmd: '%s' (try CMD:WIPE / CMD:REFRESH / CMD:STATUS)", line.c_str());
    }
  }
#endif

  // Long-press power button → sleep immediately
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    LOG_DBG("MAIN", "Long power press → deep sleep");
    HalPowerManager::Lock powerLock;
    display.deepSleep();
    powerManager.startDeepSleep(gpio);
    return;
  }

  activityManager.loop();
  delay(10);
}
