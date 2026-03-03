#pragma once
// Project-local TFT_eSPI setup (pulled in when USER_SETUP_LOADED is defined)

#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
#define TFT_BL   21

// Use HSPI for ESP32-2432S028 (non-R) CYD panels.
#define TFT_SPI_PORT HSPI

  // 55MHz is unstable on many CYD (Sunton) ILI9341 boards and can produce
  // partial-frame "snow" or tearing at screen edges.
  // 27MHz is conservative and reliable.
  #define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000

#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT
