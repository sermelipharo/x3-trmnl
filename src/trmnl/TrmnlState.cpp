#include "TrmnlState.h"

#include <Logging.h>
#include <Preferences.h>

#include "TrmnlConfig.h"

namespace {
constexpr const char* kNamespace = "trmnl";
constexpr const char* kKeyServerUrl = "server_url";
constexpr const char* kKeyApiKey = "api_key";
constexpr const char* kKeyFriendlyId = "friendly_id";
constexpr const char* kKeyLastFilename = "last_file";
constexpr const char* kKeyWakeCount = "wakes";
constexpr const char* kKeyWifiFail = "wifi_fail";
constexpr const char* kKeyPortalReq = "portal_req";
constexpr const char* kKeySetupFail = "setup_fail";
constexpr const char* kKeyLastRefresh = "last_refresh";
constexpr const char* kKeyLastOtaUrl = "last_ota_url";
}  // namespace

bool TrmnlState::load() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) {
    LOG_DBG("TRMNL", "NVS open (read) failed — first boot?");
    // Seed in-memory defaults from build flags so the first /api/setup or
    // /api/display attempt has something to work with.
    serverBaseUrl = TRMNL_DEFAULT_SERVER_URL;
    apiKey = TRMNL_DEFAULT_API_KEY;
    return false;
  }
  // Build-flag defaults seed ONLY truly uninitialized slots. Once a key
  // has been written to NVS — even to an empty string, which the captive
  // portal does when the server URL changes to force re-provisioning —
  // the stored value wins. Otherwise switching servers would silently
  // re-apply the old pre-provisioned api_key.
  if (prefs.isKey(kKeyServerUrl)) {
    serverBaseUrl = prefs.getString(kKeyServerUrl, "").c_str();
  } else {
    serverBaseUrl = TRMNL_DEFAULT_SERVER_URL;
  }
  if (prefs.isKey(kKeyApiKey)) {
    apiKey = prefs.getString(kKeyApiKey, "").c_str();
  } else {
    apiKey = TRMNL_DEFAULT_API_KEY;
  }
  friendlyId = prefs.getString(kKeyFriendlyId, "").c_str();
  lastFilename = prefs.getString(kKeyLastFilename, "").c_str();
  lastOtaUrl = prefs.getString(kKeyLastOtaUrl, "").c_str();
  wakeCount = prefs.getUInt(kKeyWakeCount, 0);
  lastRefreshRate = prefs.getUInt(kKeyLastRefresh, 0);
  wifiFailCount = prefs.getUChar(kKeyWifiFail, 0);
  setupFailCount = prefs.getUChar(kKeySetupFail, 0);
  portalRequested = prefs.getBool(kKeyPortalReq, false);
  prefs.end();
  LOG_DBG("TRMNL",
          "NVS load: server='%s' apiKey=%s friendly='%s' wakes=%u fails=%u portalReq=%d last='%s'",
          serverBaseUrl.c_str(), apiKey.empty() ? "(empty)" : "(set)", friendlyId.c_str(),
          wakeCount, wifiFailCount, (int)portalRequested, lastFilename.c_str());
  return true;
}

bool TrmnlState::save() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    LOG_ERR("TRMNL", "NVS open (write) failed");
    return false;
  }
  prefs.putString(kKeyServerUrl, serverBaseUrl.c_str());
  prefs.putString(kKeyApiKey, apiKey.c_str());
  prefs.putString(kKeyFriendlyId, friendlyId.c_str());
  prefs.putString(kKeyLastFilename, lastFilename.c_str());
  prefs.putString(kKeyLastOtaUrl, lastOtaUrl.c_str());
  prefs.putUInt(kKeyWakeCount, wakeCount);
  prefs.putUInt(kKeyLastRefresh, lastRefreshRate);
  prefs.putUChar(kKeyWifiFail, wifiFailCount);
  prefs.putUChar(kKeySetupFail, setupFailCount);
  prefs.putBool(kKeyPortalReq, portalRequested);
  prefs.end();
  return true;
}
