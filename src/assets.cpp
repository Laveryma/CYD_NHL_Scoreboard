#include "assets.h"
#include "palette.h"
#include "config.h"

#include <SPI.h>
#include <SPIFFS.h>
#include <SD.h>
#include <PNGdec.h>

namespace {

TFT_eSPI *g_tft = nullptr;

// PNGdec's main decoder class is called `PNG`.
PNG g_png;

fs::FS *g_fs = nullptr;
File g_file;

int16_t g_drawX = 0;
int16_t g_drawY = 0;

// Line buffer for PNG decode.
// 320 covers the maximum width this code will ever push (safe for rotations).
uint16_t g_line[320];

bool g_spiffsReady = false;
bool g_sdReady = false;

SPIClass g_sdVspi(VSPI);
SPIClass g_sdHspi(HSPI);

constexpr uint32_t SD_SPI_HZ = 4000000; // conservative + stable on CYD
constexpr uint32_t SD_SPI_HZ_FALLBACK = 1000000;

bool tryBeginSd(SPIClass &bus, const char *busName, uint32_t hz) {
  bus.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  const bool ok = SD.begin(SD_CS, bus, hz);
  Serial.printf("SD: %s @ %luHz -> %s\n",
                busName,
                (unsigned long)hz,
                ok ? "ready" : "fail");
  if (!ok) {
    SD.end();
  }
  return ok;
}

// --- PNGdec callbacks ---
// We keep a single global `File` (g_file) for the active decode.

void *pngOpen(const char *filename, int32_t *size) {
  if (!g_fs) return nullptr;
  g_file = g_fs->open(filename, "r");
  if (!g_file) return nullptr;
  *size = (int32_t)g_file.size();
  return (void *)1; // non-null sentinel
}

void pngClose(void *handle) {
  (void)handle;
  if (g_file) g_file.close();
}

int32_t pngRead(PNGFILE *pFile, uint8_t *pBuf, int32_t len) {
  (void)pFile;
  if (!g_file) return 0;
  return (int32_t)g_file.read(pBuf, len);
}

int32_t pngSeek(PNGFILE *pFile, int32_t position) {
  (void)pFile;
  if (!g_file) return 0;
  return (int32_t)g_file.seek(position);
}

int pngDraw(PNGDRAW *pDraw) {
  if (!g_tft) return 0;

  // TFT_eSPI expects big-endian RGB565 pixel order when swap-bytes is disabled.
  g_png.getLineAsRGB565(pDraw, g_line, PNG_RGB565_BIG_ENDIAN, 0x00000000);

  const int16_t y = (int16_t)(g_drawY + pDraw->y);
  g_tft->pushImage(g_drawX, y, pDraw->iWidth, 1, g_line);
  return 1;
}

String makeLogoPath(int16_t size, const String &abbr) {
  return String(LOGO_ROOT) + "/" + String(size) + "/" + abbr + ".png";
}

bool drawPngFromFs(fs::FS &fs, const String &path, int16_t x, int16_t y) {
  if (!g_tft) return false;

  g_fs = &fs;
  g_drawX = x;
  g_drawY = y;

  const int rcOpen = g_png.open((char *)path.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw);
  if (rcOpen != 0) {
    return false;
  }

  const int rcDec = g_png.decode(nullptr, 0);
  g_png.close();
  return (rcDec == 0);
}

} // namespace

namespace Assets {

void begin(TFT_eSPI &tft) {
  g_tft = &tft;

  // PNGdec is providing RGB565 data in big-endian order already,
  // so keep byte swapping disabled to preserve correct colours.
  g_tft->setSwapBytes(false);

  g_spiffsReady = SPIFFS.begin(true);
  Serial.println(g_spiffsReady ? "SPIFFS: ready" : "SPIFFS: FAIL");

#if ENABLE_SD_LOGOS
  // Keep CS high while probing SPI buses.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  // CYD SD wiring is typically on VSPI (SCLK=18, MISO=19, MOSI=23).
  // Try VSPI first, then HSPI for board variants.
  g_sdReady = tryBeginSd(g_sdVspi, "VSPI", SD_SPI_HZ);
  if (!g_sdReady) g_sdReady = tryBeginSd(g_sdHspi, "HSPI", SD_SPI_HZ);
  if (!g_sdReady) g_sdReady = tryBeginSd(g_sdVspi, "VSPI", SD_SPI_HZ_FALLBACK);
  if (!g_sdReady) g_sdReady = tryBeginSd(g_sdHspi, "HSPI", SD_SPI_HZ_FALLBACK);
  if (!g_sdReady) Serial.println("SD: not ready");
#else
  g_sdReady = false;
#endif
}

bool drawPng(TFT_eSPI &tft, const String &path, int16_t x, int16_t y) {
  if (!g_tft) g_tft = &tft;

  if (g_spiffsReady && SPIFFS.exists(path)) {
    if (drawPngFromFs(SPIFFS, path, x, y)) return true;
  }

#if ENABLE_SD_LOGOS
  if (g_sdReady && SD.exists(path)) {
    if (drawPngFromFs(SD, path, x, y)) return true;
  }
#endif

  return false;
}

// Forward declaration (drawLogo calls this).
void drawFallbackBadge(int16_t x, int16_t y, int size, const char *label);

void drawLogo(TFT_eSPI &tft, const String &abbr, int16_t x, int16_t y, int16_t size) {
  if (!g_tft) g_tft = &tft;

  g_tft->fillRect(x, y, size, size, Palette::BG);
  Serial.printf("drawLogo abbr=%s x=%d y=%d size=%d\n", abbr.c_str(), (int)x, (int)y, (int)size);

  bool ok = false;

#if ENABLE_SD_LOGOS
  fs::FS *first = g_sdReady ? (fs::FS *)&SD : (fs::FS *)&SPIFFS;
  const char *firstName = g_sdReady ? "SD" : "SPIFFS";
#else
  fs::FS *first = (fs::FS *)&SPIFFS;
  const char *firstName = "SPIFFS";
#endif

  const int16_t sizes[] = { size, 64, 56, 96 };
  for (int i = 0; i < 4 && !ok; ++i) {
    int16_t s = sizes[i];
    if (s <= 0) continue;
    String path = makeLogoPath(s, abbr);
    Serial.print("LOGO TRY (");
    Serial.print(firstName);
    Serial.print("): ");
    Serial.println(path);
    ok = drawPngFromFs(*first, path, x, y);
#if ENABLE_SD_LOGOS
    if (!ok && g_sdReady && first != (fs::FS *)&SPIFFS && g_spiffsReady) {
      Serial.print("LOGO TRY (SPIFFS): ");
      Serial.println(path);
      ok = drawPngFromFs(SPIFFS, path, x, y);
    } else if (!ok && g_spiffsReady && first == (fs::FS *)&SPIFFS && g_sdReady) {
      Serial.print("LOGO TRY (SD): ");
      Serial.println(path);
      ok = drawPngFromFs(SD, path, x, y);
    }
#endif
  }

  if (!ok) {
    Serial.println("LOGO FAIL (fallback)");
    drawFallbackBadge(x, y, size, abbr.c_str());
  } else {
    Serial.println("LOGO OK");
  }
}

void debugListLogos() {
  if (!g_spiffsReady) {
    Serial.println("SPIFFS not ready; cannot list logos");
    return;
  }
  Serial.println("SPIFFS: /logos present (not recursively listed)");
}

// Simple fallback if a logo isn't found/decoded.
void drawFallbackBadge(int16_t x, int16_t y, int size, const char *label) {
  if (!g_tft) return;

  const int16_t radius = (int16_t)(size / 6);
  g_tft->fillRoundRect(x, y, size, size, radius, Palette::PANEL_2);
  g_tft->drawRoundRect(x, y, size, size, radius, Palette::FRAME);

  if (label && *label) {
    g_tft->setTextDatum(MC_DATUM);
    g_tft->setTextColor(Palette::WHITE, Palette::PANEL_2);
    g_tft->setTextFont(2);
    g_tft->drawString(label, (int16_t)(x + size / 2), (int16_t)(y + size / 2));
  }
}

} // namespace Assets
