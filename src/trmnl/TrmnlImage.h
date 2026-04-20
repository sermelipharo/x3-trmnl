#pragma once
#include <string>

namespace TrmnlImage {

// Downloads an image from `url` (PNG or BMP3 1bpp), decodes it, writes
// 1bpp pixels into the e-ink framebuffer, and calls
// `display.displayBuffer()` with the chosen refresh mode. Assumes WiFi is
// already connected.
//
// Returns true only if the image was successfully fetched, decoded, and
// pushed to the panel. On any failure the framebuffer state is undefined
// and the caller should treat the cache (last-displayed filename) as stale.
//
// If `fullRefresh` is true, triggers a full e-ink refresh (~1.6 s, clears
// ghosting). Otherwise uses the fast refresh LUT (~0.6 s). Caller typically
// alternates on a wake-count policy to balance speed vs. ghosting.
bool downloadAndRender(const std::string& url, bool fullRefresh);

}  // namespace TrmnlImage
