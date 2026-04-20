#pragma once
#include <string>

// OTA firmware update from a server-provided URL. Used when /api/display
// returns update_firmware=true. Disabled by default — the BYOD contract
// does not include signature verification, so auto-apply is gated behind
// the TRMNL_ENABLE_OTA build flag at the call site.
namespace TrmnlOta {

// Streams a firmware binary from `url` into the inactive OTA partition,
// verifies size, and commits. Returns true on success (caller should
// ESP.restart()); false on any transport, size, or validation error.
//
// Caller must be on WiFi. This function blocks for the duration of the
// download + flash write (typically 10–60 s for a 1 MB firmware).
bool applyOtaUpdate(const std::string& url);

}  // namespace TrmnlOta
