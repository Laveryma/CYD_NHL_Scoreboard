<p align="center">
  <img src="https://raw.githubusercontent.com/Laveryma/CYD_NHL_Scoreboard/main/assets/Screen_BootUp.jpeg" width="180">
  <img src="https://raw.githubusercontent.com/Laveryma/CYD_NHL_Scoreboard/main/assets/Screen_NextGame.jpeg" width="180">
  <img src="https://raw.githubusercontent.com/Laveryma/CYD_NHL_Scoreboard/main/assets/Screen_GameDay.jpeg" width="180">
  <img src="https://raw.githubusercontent.com/Laveryma/CYD_NHL_Scoreboard/main/assets/Screen_LastGame.jpeg" width="180">
  <img src="https://raw.githubusercontent.com/Laveryma/CYD_NHL_Scoreboard/main/assets/Screen_Standings.jpeg" width="180">
</p> 

## CYD NHL Scoreboard

NHL scoreboard firmware for the **ESP32-2432S028 CYD** (320x240 TFT).

This project runs on ESP32 + PlatformIO and shows a focused-team game dashboard with countdown, live game state, goal alerts, final flow, recap, standings, and optional anthem playback.

## Overview

The firmware pulls data from the public NHL web API (`api-web.nhle.com`) and displays one selected team.

Key capabilities:

- Focus on a single NHL team (`FOCUS_TEAM_ABBR`) <-- update to your favoutite team, default is `TOR`
- Two Wi-Fi support (primary + fallback SSID) <-- support for on device wifi selection in next update
- Local-time scheduling with DST-aware timezone support (`TZ_INFO`)
- Countdown states: `NEXT GAME`, `GAME DAY`, `PRE-GAME`
- Live game scoreboard + stats + strength state + penalty countdown
- Goal banner with centered logo + scorer
- Final screen hold + BOOT-dismiss flow
- Last game recap screen
- Conference standings screen, split by division
- Pre-game lineup ticker for the focus team
- Optional anthem playback at puck drop transition (PRE -> LIVE)

## Screen Behavior

### Automatic mode

Auto mode routes to these screens:

- `NO_GAME` (countdown) when there is no active game or game is pre-start
- `LIVE` when game is in progress
- `INTERMISSION` during period breaks
- `FINAL` when game is complete

`LAST_GAME` and `STANDINGS` are available in manual screen cycling.

### Final dismiss flow

When a game reaches `FINAL`:

1. Final screen stays on-screen (held)
2. Bottom hint appears: `CLICK BOOT TO DISMISS`
3. BOOT short click shows `STANDINGS` for 16 seconds
4. Then screen returns to countdown (`NO_GAME`)

## Controls (BOOT button)

- **Short click (normal):** enters/cycles manual screens
  - `LAST_GAME -> STANDINGS -> LIVE -> INTERMISSION -> FINAL -> GOAL -> NO_GAME -> auto`
- **Short click on held FINAL:** dismiss flow (standings preview -> countdown)
- **Long press (~1.4s):** anthem test playback
- **During anthem playback:** short click reduces gain by 10%

## Hardware / Target

- Board: `esp32dev` (CYD ESP32-2432S028 wiring profile used in this project)
- Display: TFT_eSPI with project-local setup in `include/User_Setup.h`
- Resolution/orientation target: 320x240 landscape UI
- Audio output: ESP32 DAC (`GPIO25` or `GPIO26`) <-- tested using jst port
- Touch hardware exists on CYD, but **touch is not used** in this firmware

## Project Layout

- `src/main.cpp` - runtime loop, state logic, polling, BOOT handling
- `src/ui.cpp` - all screen rendering and layout
- `src/nhl_client.cpp` - NHL API fetch + parse
- `src/assets.cpp` - PNG loading/decoding from SPIFFS/SD
- `src/anthem.cpp` - WAV playback and puck-drop trigger
- `include/config.h` - user configuration
- `include/config.example.h` - config template for new users
- `data/` - SPIFFS content (splash, logos, audio)

## Data Source

Primary live query path:

- `https://api-web.nhle.com/v1/scoreboard/now`

Additional endpoints are used for schedule, play-by-play, boxscore, recap, and standings.

- No API key required

## Quick Start

### 1) Prerequisites

- VS Code
- PlatformIO extension
- USB data cable

### 2) Configure

Use `include/config.h`.

If needed, start from template:

```powershell
Copy-Item include\config.example.h include\config.h
```

If your local `include/config.h` already has old/private values, replace or edit it before flashing.

Minimum required settings:

- `WIFI_SSID_1`, `WIFI_PASSWORD_1`
- `FOCUS_TEAM_ABBR` (uppercase NHL team tricode, e.g. `TOR`, `EDM`, `DET`)
- `TZ_INFO` (DST-aware POSIX timezone string)
- `TFT_ROTATION` (0-3)

Recommended optional settings:

- `WIFI_SSID_2`, `WIFI_PASSWORD_2` (fallback Wi-Fi)
- `POLL_SCOREBOARD_MS`, `POLL_GAMEDETAIL_MS`
- `ANTHEM_DAC_PIN`, `ANTHEM_DAC_PIN_ALT`, `ANTHEM_GAIN_PCT`

Reference tables for team codes, timezone strings, rotation values, audio settings, and polling:

- [`assets/readme_codes.md`](assets/readme_codes.md)

### 3) Build and flash firmware

Default env is `esp32-cyd-sdfix`:

```powershell
pio run -e esp32-cyd-sdfix -t upload
```

### 4) Upload SPIFFS assets

```powershell
pio run -e esp32-cyd-sdfix -t uploadfs
```

Run `uploadfs` any time files inside `data/` change.

### 5) Serial monitor

```powershell
pio device monitor -b 115200
```

## Build Environments (Pinned vs Unpinned)

This repo is intentionally pinned for reproducible behavior in `platformio.ini`.

Default / recommended:

- `esp32-cyd-sdfix`
- pinned platform/framework/libs/toolchain
- custom partition table: `partitions_4MB_spiffs_audio.csv`

Commands:

```powershell
pio run -e esp32-cyd-sdfix
pio run -e esp32-cyd-sdfix -t upload
pio run -e esp32-cyd-sdfix -t uploadfs
```

Notes:

- `esp32-cyd-sdfix` forces `ENABLE_SD_LOGOS=0` for a stable SPIFFS-first path.
- You can build with other envs or latest defaults, but treat that as an upgrade/retest effort.

If you choose unpinned builds, retest at minimum:

- Display color order / rotation
- SPIFFS mount + asset paths
- Audio output quality/timing

## Asset Paths

### Splash

- Repo source: `data/splash.png`
- Device path: `/splash.png`

### Logos

- Repo source: `data/logos/<size>/<TEAM>.png`
- Device path: `/logos/<size>/<TEAM>.png`
- Supported runtime sizes: `56`, `64`, `96`

### Anthem

- Repo source: `data/audio/anthem.wav`
- Device path: `/audio/anthem.wav`
- Format required:
  - PCM WAV
  - Mono
  - 8-bit or 16-bit

Why both `data/logos` and `/logos`?

- `data/logos` is the source folder on your PC
- `/logos` is the runtime path inside SPIFFS after `uploadfs`

## Timezone and DST

The project uses `TZ_INFO` + `tzset()` and localtime conversion.

- DST is handled when `TZ_INFO` includes correct DST rules
- Wrong `TZ_INFO` will cause incorrect local countdown/time labels

Example (UK):

```c
#define TZ_INFO "GMT0BST,M3.5.0/1,M10.5.0/2"
```

More timezone options are listed in:

- [`assets/readme_codes.md`](assets/readme_codes.md)

## Polling Guidance

Defaults:

- `POLL_SCOREBOARD_MS = 15000`
- `POLL_GAMEDETAIL_MS = 8000`

Conservative/stable option:

- `POLL_SCOREBOARD_MS = 30000`
- `POLL_GAMEDETAIL_MS = 15000`

Avoid very low values (<5000ms) unless you explicitly test reliability.

Reference table (recommended/stable/minimum):

- [`assets/readme_codes.md`](assets/readme_codes.md)

## Audio Configuration Notes

In `include/config.h`:

- `ANTHEM_DAC_PIN`: primary DAC pin (`25` or `26`)
- `ANTHEM_DAC_PIN_ALT`: set `-1` unless mirrored DAC output is required
- `ANTHEM_GAIN_PCT`: runtime-clamped to `0..200`

Reference values:

- [`assets/readme_codes.md`](assets/readme_codes.md)

Practical gain starting range:

- 60-80 for most small amps/speakers

Symptoms:

- Too low = quiet
- Too high = clipping/distortion

## Display Color Requirement

In `include/User_Setup.h`, ensure:

```cpp
#define TFT_RGB_ORDER TFT_BGR
```

If red/blue appear swapped:

1. Confirm define exists
2. Clean and rebuild
3. Reflash

## Partition

Uses custom 4MB partition table:

- `partitions_4MB_spiffs_audio.csv`

This allocates a large SPIFFS region for logos and audio.

If `uploadfs` fails with `File system is full`:

- Reduce PNG sizes/compression
- Shorten anthem WAV duration
- Re-run `uploadfs`

## Troubleshooting

### Logos not showing

- Confirm filenames are uppercase tricodes (`TOR.png`)
- Confirm logo sizes exist (`56`, `64`, `96`)
- Re-upload SPIFFS (`uploadfs`)

### Standings/recap temporarily unavailable

- Can happen during startup or transient API/network failures
- Firmware retries automatically

### Anthem not playing

- Confirm `/audio/anthem.wav` exists (after `uploadfs`)
- Confirm WAV format is supported (PCM mono 8/16-bit)
- Confirm DAC wiring and amplifier chain
- Check serial logs for anthem pin/gain diagnostics

### Data stale / offline indications

UI can show stale/offline labels when Wi-Fi or API fetches fail.
Use serial logs to inspect HTTP status and Wi-Fi state.

## Security / GitHub Hygiene

Before publishing:

- Do **not** commit personal Wi-Fi credentials in `include/config.h`
- Keep a sanitized `config.example.h` for public use
- Keep `.pio/` build artifacts out of Git (already in `.gitignore`)

## License / Attribution

If you plan to publish publicly, add your preferred license file (for example `MIT`) and attribution notes for any third-party assets.



