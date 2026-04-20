#pragma once
#include <cstdint>
#include <string>

// Parsed response from GET /api/display. Only the fields the firmware
// acts on are surfaced here; unknown fields are ignored.
struct TrmnlDisplayResponse {
  // JSON-body status: 0 = OK (firmware convention), 202 = device not yet
  // linked on the server, 500 = invalid access token. Distinct from the
  // HTTP status code, which the caller verifies first.
  int32_t status = -1;
  std::string image_url;
  std::string filename;
  uint32_t refresh_rate = 0;
  bool reset_firmware = false;
  bool update_firmware = false;
  std::string firmware_url;
};

// Parsed response from GET /api/setup.
struct TrmnlSetupResponse {
  int32_t status = -1;          // 200 on success, 404 if MAC unknown
  std::string api_key;          // persist to NVS on success
  std::string friendly_id;      // 6-char human ID shown on splash
  std::string image_url;        // setup-complete splash image
  std::string filename;
};

namespace TrmnlApi {

// GET /api/setup with the device MAC as the ID header. Used to claim or
// re-claim an api_key when NVS has none. Returns true on transport success
// (response parsed); caller must still inspect out.status.
//
// Caller must be on WiFi before calling.
bool setup(TrmnlSetupResponse& out);

// GET /api/display with the six required BYOD headers plus Content-Type.
// Returns true on transport success; caller inspects out.status for the
// server-side outcome (0 OK, 202 not-linked, 500 invalid token).
//
// Caller must be on WiFi before calling. Does not sleep on error.
bool fetchDisplay(TrmnlDisplayResponse& out, float batteryVoltage, int rssi);

// POST /api/log — fire-and-forget diagnostic. The BYOD spec leaves the body
// schema undefined, so we send a minimal {"message": ...} object and ignore
// the response. Best-effort: logs locally on transport errors and returns
// without propagating them.
void postLog(const std::string& message);

}  // namespace TrmnlApi
