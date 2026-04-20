#include "TrmnlApi.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "TrmnlConfig.h"
#include "TrmnlState.h"

namespace TrmnlApi {

bool setup(TrmnlSetupResponse& out) {
  const std::string& baseUrl = TRMNL_STATE.getServerBaseUrl();
  if (baseUrl.empty()) {
    LOG_ERR("TRMNL", "server URL not configured — cannot run /api/setup");
    return false;
  }

  NetworkClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const std::string url = baseUrl + "/api/setup";
  if (!http.begin(client, url.c_str())) {
    LOG_ERR("TRMNL", "HTTP begin failed for %s", url.c_str());
    return false;
  }
  http.setTimeout(15000);
  http.setConnectTimeout(8000);

  const String mac = WiFi.macAddress();
  http.addHeader("ID", mac);
  // BYOS Next.js requires Model on /api/setup to auto-provision; other
  // servers ignore it. Sending unconditionally keeps the client portable.
  http.addHeader("Model", TRMNL_DEVICE_MODEL);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.GET();
  // Spec allows the server to return HTTP 200 with status:404 in the body
  // for unknown MACs; some implementations also use a real HTTP 404. Parse
  // the body in both cases so the caller can distinguish "unknown MAC"
  // from a transport error.
  if (httpCode != HTTP_CODE_OK && httpCode != 404) {
    LOG_ERR("TRMNL", "GET /api/setup returned %d", httpCode);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR("TRMNL", "setup JSON parse failed: %s (body length %u)", err.c_str(), body.length());
    return false;
  }

  out.status = doc["status"] | -1;
  // When the server returns HTTP 404 without a JSON status, normalize to
  // 404 so the caller sees an unambiguous "unknown MAC" signal.
  if (out.status < 0 && httpCode == 404) out.status = 404;
  out.api_key = doc["api_key"] | "";
  out.friendly_id = doc["friendly_id"] | "";
  out.image_url = doc["image_url"] | "";
  out.filename = doc["filename"] | "";

  LOG_INF("TRMNL", "GET /api/setup status=%d friendly_id=%s api_key=%s",
          (int)out.status, out.friendly_id.c_str(),
          out.api_key.empty() ? "(empty)" : "(set)");

  return true;
}

bool fetchDisplay(TrmnlDisplayResponse& out, float batteryVoltage, int rssi) {
  const std::string& baseUrl = TRMNL_STATE.getServerBaseUrl();
  const std::string& apiKey = TRMNL_STATE.getApiKey();
  if (baseUrl.empty()) {
    LOG_ERR("TRMNL", "server URL not configured — skipping /api/display");
    return false;
  }
  if (apiKey.empty()) {
    LOG_ERR("TRMNL", "api_key not set — device must provision via /api/setup first");
    return false;
  }

  NetworkClientSecure client;
  // Self-hosted BYOS deployments typically serve plain HTTPS without a CA
  // chain we can pin. Enable certificate validation via esp_crt_bundle_attach
  // once cloud deployments become a primary target.
  client.setInsecure();

  HTTPClient http;
  const std::string url = baseUrl + "/api/display";
  if (!http.begin(client, url.c_str())) {
    LOG_ERR("TRMNL", "HTTP begin failed for %s", url.c_str());
    return false;
  }
  // Servers that render on-demand can take longer than the default HTTP
  // timeout on a cold cache miss; cache-hit cycles finish in <1 s. 20 s
  // covers the common cases while bounding worst-case awake drain.
  http.setTimeout(20000);
  http.setConnectTimeout(8000);

  const String mac = WiFi.macAddress();
  char battVoltStr[16];
  snprintf(battVoltStr, sizeof(battVoltStr), "%.3f", batteryVoltage);
  char rssiStr[8];
  snprintf(rssiStr, sizeof(rssiStr), "%d", rssi);
  // Spec: Refresh-Rate is the *last-known* interval from the server, not a
  // static default. Falls back to the compile-time default when we haven't
  // had a successful cycle yet.
  const uint32_t lastRate = TRMNL_STATE.getLastRefreshRate();
  const uint32_t reportRate = (lastRate > 0) ? lastRate : TRMNL_DEFAULT_REFRESH_SECONDS;
  char refreshStr[16];
  snprintf(refreshStr, sizeof(refreshStr), "%u", (unsigned)reportRate);

  http.addHeader("ID", mac);
  http.addHeader("Access-Token", apiKey.c_str());
  http.addHeader("FW-Version", TRMNL_FIRMWARE_VERSION);
  http.addHeader("Model", TRMNL_DEVICE_MODEL);
  http.addHeader("Battery-Voltage", battVoltStr);
  http.addHeader("RSSI", rssiStr);
  http.addHeader("Refresh-Rate", refreshStr);
  http.addHeader("Content-Type", "application/json");

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("TRMNL", "GET /api/display returned %d", httpCode);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err) {
    LOG_ERR("TRMNL", "JSON parse failed: %s (body length %u)", err.c_str(), body.length());
    return false;
  }

  out.status = doc["status"] | -1;
  out.image_url = doc["image_url"] | "";
  out.filename = doc["filename"] | "";
  out.refresh_rate = doc["refresh_rate"] | TRMNL_DEFAULT_REFRESH_SECONDS;
  out.reset_firmware = doc["reset_firmware"] | false;
  out.update_firmware = doc["update_firmware"] | false;
  out.firmware_url = doc["firmware_url"] | "";
  LOG_INF("TRMNL", "GET /api/display OK — status=%d filename=%s refresh_rate=%u",
          (int)out.status, out.filename.c_str(), (unsigned)out.refresh_rate);

  return true;
}

void postLog(const std::string& message) {
  const std::string& baseUrl = TRMNL_STATE.getServerBaseUrl();
  const std::string& apiKey = TRMNL_STATE.getApiKey();
  if (baseUrl.empty() || apiKey.empty()) return;  // nothing sensible to send

  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const std::string url = baseUrl + "/api/log";
  if (!http.begin(client, url.c_str())) {
    LOG_DBG("TRMNL", "postLog: http.begin failed for %s (giving up)", url.c_str());
    return;
  }
  http.setTimeout(5000);
  http.setConnectTimeout(3000);

  const String mac = WiFi.macAddress();
  http.addHeader("ID", mac);
  http.addHeader("Access-Token", apiKey.c_str());
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["message"] = message;
  doc["fw_version"] = TRMNL_FIRMWARE_VERSION;
  std::string body;
  body.reserve(256);
  serializeJson(doc, body);

  const int code = http.POST(reinterpret_cast<uint8_t*>(body.data()), body.size());
  LOG_DBG("TRMNL", "postLog: /api/log returned %d (best-effort)", code);
  http.end();
}

}  // namespace TrmnlApi
