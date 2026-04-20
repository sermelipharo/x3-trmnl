#include "TrmnlImage.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <PNGdec.h>
#include <WiFiClient.h>

#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

// Cap on in-memory image buffer. Typical 1bpp TRMNL dashboards for 800x480
// panels land at ~48 KB (BMP) or 5–40 KB (PNG); 128 KB is well above the
// expected worst case while leaving ~250 KB DRAM headroom on ESP32-C3.
constexpr int kMaxImageBytes = 128 * 1024;
constexpr uint32_t kDownloadDeadlineMs = 30000;

inline uint16_t leU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t leU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Helpers to decode one pixel from the scanline into a 1-bit "keep as
// white" flag (true = leave bit as 1; false = force black).
inline bool pngPixelIsBright(const PNGDRAW* p, int x) {
  switch (p->iPixelType) {
    case PNG_PIXEL_INDEXED: {
      uint32_t idx = 0;
      if (p->iBpp == 1) {
        idx = (p->pPixels[x / 8] >> (7 - (x % 8))) & 0x01;
      } else if (p->iBpp == 2) {
        idx = (p->pPixels[x / 4] >> (6 - 2 * (x % 4))) & 0x03;
      } else if (p->iBpp == 4) {
        idx = (p->pPixels[x / 2] >> (4 - 4 * (x % 2))) & 0x0F;
      } else if (p->iBpp == 8) {
        idx = p->pPixels[x];
      } else {
        return true;
      }
      if (!p->pPalette) {
        return idx > 0;
      }
      const uint8_t r = p->pPalette[idx * 3 + 0];
      const uint8_t g = p->pPalette[idx * 3 + 1];
      const uint8_t b = p->pPalette[idx * 3 + 2];
      return (static_cast<int>(r) + g + b) >= (128 * 3);
    }
    case PNG_PIXEL_GRAYSCALE: {
      if (p->iBpp == 1) return ((p->pPixels[x / 8] >> (7 - (x % 8))) & 0x01) != 0;
      if (p->iBpp == 2) {
        const uint8_t v = (p->pPixels[x / 4] >> (6 - 2 * (x % 4))) & 0x03;
        return v >= 2;
      }
      if (p->iBpp == 4) {
        const uint8_t v = (p->pPixels[x / 2] >> (4 - 4 * (x % 2))) & 0x0F;
        return v >= 8;
      }
      if (p->iBpp == 8) return p->pPixels[x] >= 128;
      return true;
    }
    case PNG_PIXEL_GRAY_ALPHA:
      return p->pPixels[x * 2] >= 128;
    case PNG_PIXEL_TRUECOLOR: {
      const int r = p->pPixels[x * 3 + 0];
      const int g = p->pPixels[x * 3 + 1];
      const int b = p->pPixels[x * 3 + 2];
      return (r + g + b) >= (128 * 3);
    }
    case PNG_PIXEL_TRUECOLOR_ALPHA: {
      const int r = p->pPixels[x * 4 + 0];
      const int g = p->pPixels[x * 4 + 1];
      const int b = p->pPixels[x * 4 + 2];
      return (r + g + b) >= (128 * 3);
    }
  }
  return true;
}

int drawScanlineCallback(PNGDRAW* p) {
  const int screenW = display.getDisplayWidth();
  const int screenH = display.getDisplayHeight();
  const int wBytes = display.getDisplayWidthBytes();
  if (p->y < 0 || p->y >= screenH) return 1;

  uint8_t* fb = display.getFrameBuffer();
  uint8_t* line = fb + p->y * wBytes;

  const int pixelsToWrite = (p->iWidth < screenW) ? p->iWidth : screenW;
  for (int x = 0; x < pixelsToWrite; ++x) {
    const bool bright = pngPixelIsBright(p, x);
    const int bytePos = x / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x % 8));
    // SSD1677 convention (per EInkDisplay README): 1 = white, 0 = black.
    if (bright) {
      line[bytePos] |= mask;
    } else {
      line[bytePos] &= static_cast<uint8_t>(~mask);
    }
  }
  return 1;
}

bool downloadImage(const std::string& url, uint8_t*& outBuf, int& outLen) {
  NetworkClientSecure client;
  // Self-hosted BYOS deployments may serve TLS without a chain we can pin.
  // Enable cert validation via esp_crt_bundle_attach once that path matters.
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url.c_str())) {
    LOG_ERR("IMG", "http.begin failed for %s", url.c_str());
    return false;
  }
  http.setTimeout(15000);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    LOG_ERR("IMG", "image GET returned %d", code);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0 || contentLength > kMaxImageBytes) {
    LOG_ERR("IMG", "invalid image size: %d (max %d)", contentLength, kMaxImageBytes);
    http.end();
    return false;
  }

  auto* buf = static_cast<uint8_t*>(malloc(contentLength));
  if (!buf) {
    LOG_ERR("IMG", "malloc %d failed", contentLength);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int read = 0;
  const uint32_t deadline = millis() + kDownloadDeadlineMs;
  while (read < contentLength && millis() < deadline) {
    const int avail = stream->available();
    if (avail > 0) {
      const int n = stream->readBytes(buf + read, contentLength - read);
      if (n <= 0) break;
      read += n;
    } else {
      delay(2);
    }
  }
  http.end();

  if (read != contentLength) {
    LOG_ERR("IMG", "download incomplete: %d/%d bytes", read, contentLength);
    free(buf);
    return false;
  }

  outBuf = buf;
  outLen = contentLength;
  return true;
}

bool decodePngToFramebuffer(const uint8_t* buf, int len) {
  // PNG object holds an internal scanline buffer sized by PNG_MAX_BUFFERED_PIXELS.
  // Even at the tuned value (800 px) the struct is a few KB — heap it anyway
  // so stack usage in loopTask stays bounded.
  auto png = std::unique_ptr<PNG>(new PNG());
  int rc = png->openRAM(const_cast<uint8_t*>(buf), len, drawScanlineCallback);
  if (rc != PNG_SUCCESS) {
    LOG_ERR("IMG", "PNG openRAM failed (rc=%d)", rc);
    return false;
  }

  const int pngW = png->getWidth();
  const int pngH = png->getHeight();
  LOG_INF("IMG", "PNG %dx%d bpp=%d type=%d", pngW, pngH, png->getBpp(), png->getPixelType());

  const int screenW = display.getDisplayWidth();
  const int screenH = display.getDisplayHeight();
  if (pngW != screenW || pngH != screenH) {
    LOG_ERR("IMG", "dimension mismatch: image %dx%d vs display %dx%d (will partial-render)",
            pngW, pngH, screenW, screenH);
    // Continue — the callback clips lines at screen bounds.
  }

  rc = png->decode(nullptr, 0);
  if (rc != PNG_SUCCESS) {
    LOG_ERR("IMG", "PNG decode failed (rc=%d)", rc);
    return false;
  }
  return true;
}

// Decodes a BMP3 1bpp bitmap (the BYOD canonical format) directly into the
// framebuffer. Supports top-down or bottom-up row order and uses the color
// table's luminance to decide which palette index maps to "white" on the
// SSD1677-style panel (1 = white, 0 = black).
bool decodeBmp1bppToFramebuffer(const uint8_t* buf, int len) {
  if (len < 62) {
    LOG_ERR("IMG", "BMP too short: %d bytes", len);
    return false;
  }
  if (buf[0] != 'B' || buf[1] != 'M') {
    LOG_ERR("IMG", "BMP signature missing");
    return false;
  }
  const uint32_t pixelOffset = leU32(buf + 10);
  const uint32_t dibSize = leU32(buf + 14);
  if (dibSize < 40) {
    LOG_ERR("IMG", "Unsupported DIB header size: %u", (unsigned)dibSize);
    return false;
  }
  const int32_t widthSigned = static_cast<int32_t>(leU32(buf + 18));
  const int32_t heightSigned = static_cast<int32_t>(leU32(buf + 22));
  const uint16_t bpp = leU16(buf + 28);
  const uint32_t compression = leU32(buf + 30);

  if (bpp != 1) {
    LOG_ERR("IMG", "BMP bpp=%u unsupported (expected 1)", (unsigned)bpp);
    return false;
  }
  if (compression != 0) {
    LOG_ERR("IMG", "BMP compression=%u unsupported (expected BI_RGB)", (unsigned)compression);
    return false;
  }
  if (pixelOffset + 1 >= static_cast<uint32_t>(len)) {
    LOG_ERR("IMG", "BMP pixel offset %u out of range (len=%d)", (unsigned)pixelOffset, len);
    return false;
  }

  const int bmpW = widthSigned;
  const int bmpH = (heightSigned < 0) ? -heightSigned : heightSigned;
  const bool topDown = heightSigned < 0;
  const int screenW = display.getDisplayWidth();
  const int screenH = display.getDisplayHeight();

  // Reject only malformed (non-positive) dimensions. BYOS servers built
  // for upstream TRMNL panels (800x480) routinely send images larger than
  // a custom panel's resolution; we clip at write-time and leave
  // uncovered regions white (clearScreen(0xFF) above the decoder call).
  if (bmpW <= 0 || bmpH <= 0) {
    LOG_ERR("IMG", "BMP dimensions invalid: %dx%d", bmpW, bmpH);
    return false;
  }

  // BMP rows are padded to a 4-byte boundary.
  const int rowBytes = ((bmpW + 31) / 32) * 4;

  // Validate the full pixel region fits inside the downloaded buffer
  // before we start writing to the framebuffer.
  const uint64_t pixelSpan = static_cast<uint64_t>(rowBytes) * bmpH;
  if (static_cast<uint64_t>(pixelOffset) + pixelSpan > static_cast<uint64_t>(len)) {
    LOG_ERR("IMG", "BMP pixel region %llu exceeds buffer (offset=%u len=%d)",
            (unsigned long long)pixelSpan, (unsigned)pixelOffset, len);
    return false;
  }

  // Palette is 2 BGRA entries at offset 14 + dibSize. Decide which index
  // represents "bright" (stays 1=white on the panel).
  const uint32_t paletteOffset = 14 + dibSize;
  bool indexBright[2] = {false, true};
  if (paletteOffset + 8 <= static_cast<uint32_t>(len)) {
    for (int i = 0; i < 2; ++i) {
      const int b = buf[paletteOffset + i * 4 + 0];
      const int g = buf[paletteOffset + i * 4 + 1];
      const int r = buf[paletteOffset + i * 4 + 2];
      indexBright[i] = (r + g + b) >= (128 * 3);
    }
  }

  if (bmpW != screenW || bmpH != screenH) {
    LOG_INF("IMG", "BMP dimension mismatch: %dx%d vs display %dx%d (clipping)",
            bmpW, bmpH, screenW, screenH);
  }
  LOG_INF("IMG", "BMP %dx%d bpp=1 topDown=%d rowBytes=%d", bmpW, bmpH, (int)topDown, rowBytes);

  uint8_t* fb = display.getFrameBuffer();
  const int fbRowBytes = display.getDisplayWidthBytes();
  const int pixelsToWrite = (bmpW < screenW) ? bmpW : screenW;
  const int rowsToWrite = (bmpH < screenH) ? bmpH : screenH;

  for (int srcRow = 0; srcRow < rowsToWrite; ++srcRow) {
    // In bottom-up BMPs, the first row in the file is the last row on screen.
    const int dstRow = topDown ? srcRow : (bmpH - 1 - srcRow);
    if (dstRow < 0 || dstRow >= screenH) continue;

    const uint32_t rowStart = pixelOffset + srcRow * rowBytes;
    if (rowStart + rowBytes > static_cast<uint32_t>(len)) {
      LOG_ERR("IMG", "BMP truncated at row %d", srcRow);
      return false;
    }
    const uint8_t* srcRowPtr = buf + rowStart;
    uint8_t* dstRowPtr = fb + dstRow * fbRowBytes;

    for (int x = 0; x < pixelsToWrite; ++x) {
      const uint8_t idx = (srcRowPtr[x / 8] >> (7 - (x % 8))) & 0x01;
      const uint8_t mask = static_cast<uint8_t>(0x80 >> (x % 8));
      if (indexBright[idx]) {
        dstRowPtr[x / 8] |= mask;
      } else {
        dstRowPtr[x / 8] &= static_cast<uint8_t>(~mask);
      }
    }
  }
  return true;
}

}  // namespace

namespace TrmnlImage {

bool downloadAndRender(const std::string& url, bool fullRefresh) {
  uint8_t* buf = nullptr;
  int len = 0;
  if (!downloadImage(url, buf, len)) {
    return false;
  }
  LOG_INF("IMG", "Downloaded %d bytes — decoding", len);

  // Clear framebuffer to white so uncovered regions stay clean regardless
  // of which decoder fires.
  display.clearScreen(0xFF);

  bool decoded = false;
  if (len >= 2 && buf[0] == 'B' && buf[1] == 'M') {
    decoded = decodeBmp1bppToFramebuffer(buf, len);
  } else if (len >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' &&
             buf[3] == 'G') {
    decoded = decodePngToFramebuffer(buf, len);
  } else {
    LOG_ERR("IMG", "Unknown image format (first bytes: %02x %02x %02x %02x)",
            len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0,
            len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0);
  }

  free(buf);
  buf = nullptr;

  if (!decoded) return false;

  display.displayBuffer(fullRefresh ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  LOG_INF("IMG", "Displayed OK (%s refresh)", fullRefresh ? "full" : "fast");
  return true;
}

}  // namespace TrmnlImage
