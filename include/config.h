#pragma once

// Wi-Fi (primary + fallback).
// Device prefers WIFI_SSID_1 when it is visible, otherwise it will try WIFI_SSID_2.
// Set WIFI_SSID_2 to "" to disable fallback.
#define WIFI_SSID_1       ""
#define WIFI_PASSWORD_1   ""

#define WIFI_SSID_2       ""
#define WIFI_PASSWORD_2   ""

// Connection behaviour
#define WIFI_SCAN_BEFORE_CONNECT      1
#define WIFI_CONNECT_TIMEOUT_MS       15000
#define WIFI_RECONNECT_INTERVAL_MS    30000

// Optional: if connected to the fallback, periodically roam back to primary when it returns.
// Set to 0 to disable.
#define WIFI_ROAM_TO_PRIMARY          0
#define WIFI_ROAM_CHECK_INTERVAL_MS   120000

// Screen rotation (TFT_eSPI setRotation):
// 0=portrait, 1=landscape, 2=portrait (inverted), 3=landscape (inverted)
#define TFT_ROTATION 1

// Team focus: default - Toronto Maple Leafs - update using Team abreviation to focus on a different team (e.g. "NYR" for 'New York Rangers').
// Note: See 'assets\README_CODES.md' for list of valid team abbreviations
#define FOCUS_TEAM_ABBR "TOR"

// Poll intervals (ms)
#define POLL_SCOREBOARD_MS   15000   // 15s
#define POLL_GAMEDETAIL_MS   8000    // 8s (only when a game is live)

// --- microSD team logos (PNG) ---
// Recommended folder layout on the SD card (case-sensitive):
//   /logos/56/TOR.png
//   /logos/64/TOR.png
//   /logos/96/TOR.png
// Enable SD logo support
#ifndef ENABLE_SD_LOGOS
#define ENABLE_SD_LOGOS 1
#endif

// CYD ESP32-2432S028 microSD pins (usually VSPI wiring)
#ifndef SD_CS
#define SD_CS   5
#endif
#ifndef SD_SCLK
#define SD_SCLK 18
#endif
#ifndef SD_MISO
#define SD_MISO 19
#endif
#ifndef SD_MOSI
#define SD_MOSI 23
#endif

// Logo folder prefix on SD card
#ifndef LOGO_ROOT
#define LOGO_ROOT "/logos"
#endif

// CYD backlight PWM channel
#define CYD_BL_PWM_CH 0

// BOOT button (GPIO0) for screen cycling.
#define BOOT_BTN_PIN 0

// --- Time + countdown ---
// POSIX TZ string for Europe/London (DST aware). You can change this if you want
// countdowns and times shown in a different local timezone.
#define TZ_INFO "GMT0BST,M3.5.0/1,M10.5.0/2"

#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

// DAC pin used for anthem playback (ESP32 DAC-capable pins: 25 or 26)
#ifndef ANTHEM_DAC_PIN
#define ANTHEM_DAC_PIN 26
#endif

// Optional secondary DAC pin for CYD wiring variants.
// Set to -1 for single-DAC mode (recommended default).
// Set to 25 or 26 only if your board wiring requires mirrored output.
#ifndef ANTHEM_DAC_PIN_ALT
#define ANTHEM_DAC_PIN_ALT -1
#endif

// Anthem output gain in percent. Increase if audio is too quiet.
// Runtime clamps this to 0..200 to avoid extreme clipping.
#ifndef ANTHEM_GAIN_PCT
#define ANTHEM_GAIN_PCT 60
#endif
