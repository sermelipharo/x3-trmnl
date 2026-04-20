#pragma once
#include "../Activity.h"

// Soft-AP + DNS hijack + HTTP form for first-time WiFi setup.
//
// Shown when WifiCredentialStore is empty at boot. Puts the device in AP
// mode (SSID "TRMNL-XXXXXX" from MAC suffix, open), runs a DNS server that
// returns the device's own IP for every query, and serves a captive-portal
// page with an SSID dropdown + password field. On form submit, saves creds
// to the store and restarts into normal TRMNL mode.
//
// preventAutoSleep = true: never time-out during setup.
// skipLoopDelay = true: webserver/DNS need fast-path polling.
class CaptivePortalActivity final : public Activity {
 public:
  explicit CaptivePortalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CaptivePortal", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  void renderInstructions();
};
