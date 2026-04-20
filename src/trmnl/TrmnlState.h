#pragma once
#include <cstdint>
#include <string>

// NVS-backed state persisted across deep sleep. The MCU is fully powered
// down between wakes (MOSFET-off topology), so RTC_DATA_ATTR memory does
// not retain — anything needed on the next wake lives here, in flash.
//
// Keep this small: each key costs ~2–5 ms to read and flash erase cycles
// are finite. Use save() only after an actual field change.
class TrmnlState {
 public:
  // Load all fields from NVS. Returns false on first boot (namespace
  // missing). Fields retain their defaults when load fails.
  bool load();

  // Persist current fields to NVS.
  bool save();

  // --- BYOD provisioning ---------------------------------------------------

  // Server base URL (scheme + host, no trailing slash). Empty means use the
  // compile-time default; both empty means the device cannot fetch and
  // should fall back to captive portal.
  const std::string& getServerBaseUrl() const { return serverBaseUrl; }
  void setServerBaseUrl(const std::string& v) { serverBaseUrl = v; }

  // API key returned by /api/setup. Empty means unprovisioned — the device
  // must run the setup flow before /api/display is reachable.
  const std::string& getApiKey() const { return apiKey; }
  void setApiKey(const std::string& v) { apiKey = v; }

  // Friendly ID (6-char human identifier) returned by /api/setup. Shown on
  // the splash screen so the user can link the device in the server UI.
  const std::string& getFriendlyId() const { return friendlyId; }
  void setFriendlyId(const std::string& v) { friendlyId = v; }

  // --- Display cache ------------------------------------------------------

  // Filename of the last successfully rendered image. Cache-hit fast path:
  // when /api/display returns the same filename, skip download + decode +
  // refresh entirely.
  const std::string& getLastFilename() const { return lastFilename; }
  void setLastFilename(const std::string& f) { lastFilename = f; }

  // --- Runtime counters ---------------------------------------------------

  // Monotonic wake counter. Forces a periodic FULL_REFRESH and helps with
  // field diagnostics.
  uint32_t getWakeCount() const { return wakeCount; }
  void incrementWakeCount() { wakeCount++; }

  // Consecutive WiFi failures. Reset to 0 on any successful connect. When
  // it reaches WIFI_FAIL_THRESHOLD the stored credentials are wiped and
  // the device reboots into the captive portal.
  static constexpr uint8_t WIFI_FAIL_THRESHOLD = 10;
  uint8_t getWifiFailCount() const { return wifiFailCount; }
  void setWifiFailCount(uint8_t v) { wifiFailCount = v; }

  // Consecutive /api/setup failures (transport error, 404 unknown MAC, etc.).
  // Used for exponential backoff so a misconfigured server doesn't drain the
  // battery retrying every 15 min. Reset to 0 on a successful setup.
  uint8_t getSetupFailCount() const { return setupFailCount; }
  void setSetupFailCount(uint8_t v) { setupFailCount = v; }

  // Last server-supplied refresh_rate (seconds). Reported back in the
  // Refresh-Rate header so the server can reason about device cadence.
  // 0 means "not yet known" — device falls back to the compile-time default.
  uint32_t getLastRefreshRate() const { return lastRefreshRate; }
  void setLastRefreshRate(uint32_t v) { lastRefreshRate = v; }

  // Absolute URL of the firmware image most recently applied via OTA.
  // Used to break the "server always says update_firmware=true" loop —
  // if the response repeats the same URL we skip instead of reflashing
  // every wake.
  const std::string& getLastOtaUrl() const { return lastOtaUrl; }
  void setLastOtaUrl(const std::string& v) { lastOtaUrl = v; }

  // Set when a credential wipe decides the next boot must go to captive
  // portal regardless of any build-flag bootstrap. Cleared on Save inside
  // the portal.
  bool isPortalRequested() const { return portalRequested; }
  void setPortalRequested(bool v) { portalRequested = v; }

  static TrmnlState& getInstance() {
    static TrmnlState instance;
    return instance;
  }

 private:
  TrmnlState() = default;
  std::string serverBaseUrl;
  std::string apiKey;
  std::string friendlyId;
  std::string lastFilename;
  std::string lastOtaUrl;
  uint32_t wakeCount = 0;
  uint32_t lastRefreshRate = 0;
  uint8_t wifiFailCount = 0;
  uint8_t setupFailCount = 0;
  bool portalRequested = false;
};

#define TRMNL_STATE TrmnlState::getInstance()
