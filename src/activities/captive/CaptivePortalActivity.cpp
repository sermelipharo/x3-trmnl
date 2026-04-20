#include "CaptivePortalActivity.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <WebServer.h>
#include <WiFi.h>

#include <memory>

#include "../../WifiCredentialStore.h"
#include "../../fontIds.h"
#include "../../trmnl/TrmnlState.h"

namespace {

// Singletons held across the activity's lifetime. Allocated on heap to keep
// the activity's stack footprint small (WebServer/DNSServer carry internal
// buffers that don't play well with the 8 KB Arduino loop stack).
std::unique_ptr<DNSServer> dns;
std::unique_ptr<WebServer> web;
String apSsid;

// Captive-probe endpoints: Android, iOS, Windows. They must return a
// redirect to our root page so the phone's OS renders the portal UI.
const char* kProbePaths[] = {
    "/generate_204",     // Android
    "/gen_204",          // Android variant
    "/hotspot-detect.html",   // iOS
    "/library/test/success.html",  // iOS variant
    "/ncsi.txt",         // Windows
    "/connecttest.txt",  // Windows
};

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;"; break;
      case '<':  out += "&lt;"; break;
      case '>':  out += "&gt;"; break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:   out += c;
    }
  }
  return out;
}

String buildForm(const String& message = "") {
  String html;
  html.reserve(4096);
  html += R"(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>TRMNL Setup</title>
<style>
body { font: 16px/1.4 -apple-system, system-ui, sans-serif; margin: 0; padding: 20px; background: #f5f5f5; }
.card { max-width: 480px; margin: 40px auto; background: white; padding: 28px; border-radius: 14px; box-shadow: 0 2px 12px rgba(0,0,0,.08); }
h1 { margin: 0 0 6px; font-size: 22px; }
p { margin: 0 0 18px; color: #555; }
label { display: block; margin: 14px 0 6px; font-weight: 600; }
input, select { width: 100%; padding: 10px; border: 1px solid #ccc; border-radius: 8px; font-size: 16px; box-sizing: border-box; }
button { margin-top: 22px; width: 100%; padding: 12px; background: #000; color: white; border: 0; border-radius: 8px; font-size: 16px; font-weight: 600; cursor: pointer; }
.msg { margin-bottom: 18px; padding: 10px 12px; background: #fff7d6; border: 1px solid #f0d872; border-radius: 8px; }
.err { background: #ffd6d6; border-color: #f08585; }
</style></head><body>
<div class="card">
<h1>TRMNL setup</h1>
<p>Pick a WiFi network, enter the password, and optionally point the device at your BYOS server.</p>)";
  if (message.length() > 0) {
    html += "<div class=\"msg\">";
    html += htmlEscape(message);
    html += "</div>";
  }
  html += R"(<form method="POST" action="/save">
<label for="ssid">Network</label>
<select name="ssid" id="ssid">)";

  const int n = WiFi.scanComplete();
  if (n > 0) {
    // Build set of unique SSIDs, pick strongest RSSI per SSID.
    for (int i = 0; i < n; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      html += "<option value=\"";
      html += htmlEscape(ssid);
      html += "\">";
      html += htmlEscape(ssid);
      html += " (";
      html += String(WiFi.RSSI(i));
      html += " dBm)</option>";
    }
  } else {
    html += "<option value=\"\">(scanning... refresh in a moment)</option>";
  }
  html += R"(</select>
<label for="pwd">Password</label>
<input type="password" name="pwd" id="pwd" autocomplete="off">
<label for="server">Server URL <span style="font-weight:400;color:#888">(optional)</span></label>
<input type="url" name="server" id="server" placeholder="https://your-byos-host" value=")";
  html += htmlEscape(String(TRMNL_STATE.getServerBaseUrl().c_str()));
  html += R"(" autocomplete="off">
<button type="submit">Save & reboot</button>
</form>
</div></body></html>)";
  return html;
}

void sendForm(const String& message = "") {
  web->send(200, "text/html; charset=utf-8", buildForm(message));
}

void handleRoot() { sendForm(); }

void handleSave() {
  if (!web->hasArg("ssid") || !web->hasArg("pwd")) {
    sendForm("Missing SSID or password.");
    return;
  }
  const String ssid = web->arg("ssid");
  const String pwd = web->arg("pwd");
  if (ssid.length() == 0) {
    sendForm("SSID is empty. Try again.");
    return;
  }

  LOG_INF("CAP", "Saving creds for SSID='%s' (%d char password)", ssid.c_str(), (int)pwd.length());
  WIFI_STORE.addCredential(std::string(ssid.c_str()), std::string(pwd.c_str()));
  // Mark this SSID as the preferred one so TrmnlActivity's connectWifi picks
  // it on the next boot instead of an older bootstrap-era credential.
  WIFI_STORE.setLastConnectedSsid(std::string(ssid.c_str()));
  WIFI_STORE.saveToFile();

  // Optional server URL override. Empty input keeps whatever is already in
  // NVS (or the build-flag default). Setting it here also wipes any stored
  // api_key + friendly_id so the next boot re-runs /api/setup against the
  // new host — otherwise we'd keep authenticating against the old server.
  if (web->hasArg("server")) {
    String rawServer = web->arg("server");
    rawServer.trim();
    while (rawServer.endsWith("/")) rawServer.remove(rawServer.length() - 1, 1);
    if (rawServer.length() > 0) {
      // Reject anything that isn't a plain http(s) base URL. A typo like
      // "htps://..." or a bare hostname otherwise gets persisted and then
      // every wake fails /api/setup, which the exponential-backoff path
      // catches but the user experience is awful.
      const bool isHttps = rawServer.startsWith("https://");
      const bool isHttp = rawServer.startsWith("http://");
      const int schemeLen = isHttps ? 8 : (isHttp ? 7 : 0);
      const bool hasHost = schemeLen > 0 && rawServer.length() > schemeLen &&
                           rawServer.indexOf('/', schemeLen) < 0 &&
                           rawServer.indexOf(' ') < 0;
      if (!hasHost) {
        LOG_ERR("CAP", "Rejecting invalid server URL: '%s'", rawServer.c_str());
        sendForm("Server URL must be http(s)://host[:port] with no path.");
        return;
      }
      const std::string newUrl = rawServer.c_str();
      if (newUrl != TRMNL_STATE.getServerBaseUrl()) {
        LOG_INF("CAP", "Server URL changed → %s (re-provisioning on next boot)",
                newUrl.c_str());
        TRMNL_STATE.setServerBaseUrl(newUrl);
        TRMNL_STATE.setApiKey("");
        TRMNL_STATE.setFriendlyId("");
        TRMNL_STATE.setSetupFailCount(0);
      }
    }
  }
  // Clear the "portal requested" marker so the next boot proceeds into the
  // normal TRMNL cycle instead of bouncing back into the portal.
  TRMNL_STATE.setPortalRequested(false);
  TRMNL_STATE.save();

  String ok;
  ok.reserve(512);
  ok += R"(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Saved</title>
<style>body{font:16px/1.4 -apple-system,system-ui,sans-serif;margin:0;padding:40px;text-align:center;background:#f5f5f5}</style>
</head><body><h1>✅ Saved</h1>
<p>The device will reboot and try to connect to <b>)";
  ok += htmlEscape(ssid);
  ok += R"(</b> in a few seconds.</p></body></html>)";
  web->send(200, "text/html; charset=utf-8", ok);
  web->client().flush();

  delay(1000);
  ESP.restart();
}

void handleCaptiveProbe() {
  // Android/iOS expect a redirect to any URL that's clearly not their probe
  // success URL. Sending 302 to our root triggers the captive-portal UI.
  web->sendHeader("Location", "http://192.168.4.1/", true);
  web->send(302, "text/plain", "");
}

}  // namespace

void CaptivePortalActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("CAP", "Starting captive portal");

  // WiFi stack must be up before MAC is valid — otherwise macAddress()
  // returns all zeros and the AP SSID degenerates to "TRMNL-000000".
  WiFi.mode(WIFI_AP_STA);

  // Derive AP SSID from MAC suffix for uniqueness in multi-device households.
  const String mac = WiFi.macAddress();
  apSsid = "TRMNL-";
  apSsid += mac.substring(9);  // "AA:BB:CC:DD:EE:FF" → "DD:EE:FF"
  apSsid.replace(":", "");

  WiFi.softAP(apSsid.c_str());
  WiFi.scanNetworks(true /* async */, true /* show hidden */);

  const IPAddress apIp = WiFi.softAPIP();
  LOG_INF("CAP", "AP '%s' up, ip=%s", apSsid.c_str(), apIp.toString().c_str());

  dns.reset(new DNSServer());
  dns->start(53, "*", apIp);

  web.reset(new WebServer(80));
  web->on("/", HTTP_GET, handleRoot);
  web->on("/save", HTTP_POST, handleSave);
  for (const char* p : kProbePaths) {
    web->on(p, HTTP_GET, handleCaptiveProbe);
  }
  web->onNotFound(handleCaptiveProbe);
  web->begin();

  renderInstructions();
}

void CaptivePortalActivity::onExit() {
  if (web) web->close();
  web.reset();
  if (dns) dns->stop();
  dns.reset();
  WiFi.softAPdisconnect(true);
  Activity::onExit();
}

void CaptivePortalActivity::loop() {
  Activity::loop();
  if (dns) dns->processNextRequest();
  if (web) web->handleClient();
  delay(1);  // yield to FreeRTOS; not too long — webserver needs responsiveness
}

void CaptivePortalActivity::renderInstructions() {
  // Plain-text instructions. No theme/icons — we're an appliance, the phone
  // does all the real UI work via the captive-portal browser.
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 60, "TRMNL setup", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20,
                            (String("Join WiFi: ") + apSsid).c_str(), true);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 10,
                            "Then open http://192.168.4.1");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 40,
                            "The page should pop up automatically.");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
