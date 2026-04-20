#include "TrmnlOta.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <WiFiClient.h>

namespace TrmnlOta {

bool applyOtaUpdate(const std::string& url) {
  NetworkClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url.c_str())) {
    LOG_ERR("OTA", "http.begin failed for %s", url.c_str());
    return false;
  }
  http.setTimeout(60000);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    LOG_ERR("OTA", "firmware GET returned %d", code);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    LOG_ERR("OTA", "firmware has unknown content length — refusing (server should set Content-Length)");
    http.end();
    return false;
  }
  LOG_INF("OTA", "Downloading %d bytes from %s", contentLength, url.c_str());

  if (!Update.begin(contentLength)) {
    LOG_ERR("OTA", "Update.begin failed: %s", Update.errorString());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  const size_t written = Update.writeStream(*stream);
  if (static_cast<int>(written) != contentLength) {
    LOG_ERR("OTA", "partial write: %u / %d bytes (%s)", (unsigned)written,
            contentLength, Update.errorString());
    Update.abort();
    http.end();
    return false;
  }

  if (!Update.end(/*evenIfRemaining=*/true)) {
    LOG_ERR("OTA", "Update.end failed: %s", Update.errorString());
    http.end();
    return false;
  }
  http.end();

  if (!Update.isFinished()) {
    LOG_ERR("OTA", "Update not finished after end()");
    return false;
  }

  LOG_INF("OTA", "OTA written successfully — caller should ESP.restart()");
  return true;
}

}  // namespace TrmnlOta
