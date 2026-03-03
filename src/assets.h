#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Optional SD-logo support is configured via include/config.h.
// This header stays lightweight; implementation lives in assets.cpp.

namespace Assets {

// Call once after TFT is initialised.
void begin(TFT_eSPI &tft);

// Draw an image from SPIFFS/SD at x,y (top-left). Returns true on success.
bool drawPng(TFT_eSPI &tft, const String &path, int16_t x, int16_t y);

// Draw a team logo at x,y (top-left). If a PNG logo exists on SD it will be used,
// otherwise a simple badge fallback is drawn.
//
// Expected logo paths on the SD card:
//   <LOGO_ROOT>/<size>/<ABBR>.png
// Example:
//   /logos/56/TOR.png
void drawLogo(TFT_eSPI &tft, const String &abbr, int16_t x, int16_t y, int16_t size = 56);

// For diagnostics.
bool sdReady();

} // namespace Assets
