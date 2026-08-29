#pragma once

// Compile-time defaults for the TRMNL BYOD client.
//
// Runtime values are read from NVS (see TrmnlState). These macros only
// seed the *initial* value used when NVS is empty (first boot or after a
// wipe). Empty strings are valid: an empty TRMNL_API_KEY means the device
// must provision itself via GET /api/setup on first wake.

// Default server base URL used until the captive portal or a build override
// provides one. Leave empty to force the user to enter a URL on first boot.
#ifndef TRMNL_DEFAULT_SERVER_URL
#define TRMNL_DEFAULT_SERVER_URL ""
#endif

// Pre-provisioned api_key for development / flashing an already-registered
// device. Production builds leave this empty so the device runs the
// /api/setup flow on first boot.
#ifndef TRMNL_DEFAULT_API_KEY
#define TRMNL_DEFAULT_API_KEY ""
#endif

// Device model string sent in the Model header. Required by BYOS Next.js
// for /api/setup auto-provisioning; other BYOS implementations ignore it
// but accept it. Keep short; some servers surface this in their admin UI.
#ifndef TRMNL_DEVICE_MODEL
#define TRMNL_DEVICE_MODEL "byod"
#endif

// Default refresh interval when the server response omits refresh_rate.
#ifndef TRMNL_DEFAULT_REFRESH_SECONDS
#define TRMNL_DEFAULT_REFRESH_SECONDS 1800
#endif

// WiFi connect timeout per attempt (ms). Short timeouts preserve battery —
// every second awake on battery costs ~22 µAh.
#ifndef TRMNL_WIFI_CONNECT_TIMEOUT_MS
#define TRMNL_WIFI_CONNECT_TIMEOUT_MS 15000
#endif

// Device firmware version string reported in the FW-Version header. BYOS
// servers compare this against their configured firmware_download_url to
// decide whether to flag update_firmware:true in /api/display responses.
#ifndef TRMNL_FIRMWARE_VERSION
#define TRMNL_FIRMWARE_VERSION "0.2.0"
#endif
