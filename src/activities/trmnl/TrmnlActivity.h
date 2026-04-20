#pragma once
#include <string>

#include "../Activity.h"

class TrmnlActivity final : public Activity {
 public:
  explicit TrmnlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TRMNL", renderer, mappedInput) {}
  void onEnter() override;

 private:
  // Connect to WiFi using stored credentials. Returns true on successful
  // association + IP. Timeout from TrmnlConfig.h.
  bool connectWifi();

  // Runs the full cycle once — WiFi connect, provisioning or display fetch,
  // image render, deep sleep. Never returns.
  [[noreturn]] void runCycleAndSleep();

  // Text-only splash rendered after /api/setup succeeds when the server
  // did not include a setup image URL (rare for compliant BYOS).
  void renderSetupSplash(const std::string& friendlyId);

  // Text-only splash rendered when /api/setup fails (MAC unknown on server,
  // transport error, etc.). Device will retry on next wake.
  void renderSetupFailure();

  // Shown when /api/display returns status=202 (device known but not yet
  // linked to a playlist in the server UI).
  void renderPendingLink();

  // Shown when /api/display returns a server-side error (500 etc.).
  void renderServerError(int32_t status);
};
