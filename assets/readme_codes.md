# README_CODES

Reference tables for `include/config.h` values.

Source: `Hockey program codes.xlsx` (provided by repo owner).

## FOCUS_TEAM_ABBR Codes

Use these uppercase team codes in:

```c
#define FOCUS_TEAM_ABBR "TOR"
```

| Team Name | Code |
|---|---|
| Anaheim Ducks | ANA |
| Boston Bruins | BOS |
| Buffalo Sabres | BUF |
| Carolina Hurricanes | CAR |
| Columbus Blue Jackets | CBJ |
| Calgary Flames | CGY |
| Chicago Blackhawks | CHI |
| Colorado Avalanche | COL |
| Dallas Stars | DAL |
| Detroit Red Wings | DET |
| Edmonton Oilers | EDM |
| Florida Panthers | FLA |
| Los Angeles Kings | LAK |
| Minnesota Wild | MIN |
| Montreal Canadiens | MTL |
| New Jersey Devils | NJD |
| Nashville Predators | NSH |
| New York Islanders | NYI |
| New York Rangers | NYR |
| Ottawa Senators | OTT |
| Philadelphia Flyers | PHI |
| Pittsburgh Penguins | PIT |
| Seattle Kraken | SEA |
| San Jose Sharks | SJS |
| St. Louis Blues | STL |
| Tampa Bay Lightning | TBL |
| Toronto Maple Leafs | TOR |
| Utah Hockey Club | UTA |
| Vancouver Canucks | VAN |
| Vegas Golden Knights | VGK |
| Winnipeg Jets | WPG |
| Washington Capitals | WSH |

## TZ_INFO Codes

Use these values in:

```c
#define TZ_INFO "GMT0BST,M3.5.0/1,M10.5.0/2"
```

| Region | TZ_INFO Value |
|---|---|
| UK (London) | `GMT0BST,M3.5.0/1,M10.5.0` |
| CET (Paris/Berlin/Rome) | `CET-1CEST,M3.5.0/2,M10.5.0/3` |
| EET (Helsinki/Athens) | `EET-2EEST,M3.5.0/3,M10.5.0/4` |
| US Eastern | `EST5EDT,M3.2.0/2,M11.1.0/2` |
| US Central | `CST6CDT,M3.2.0/2,M11.1.0/2` |
| US Mountain | `MST7MDT,M3.2.0/2,M11.1.0/2` |
| US Pacific | `PST8PDT,M3.2.0/2,M11.1.0/2` |
| Arizona (no DST) | `MST7` |
| Alaska | `AKST9AKDT,M3.2.0/2,M11.1.0/2` |
| Hawaii | `HST10` |
| Sydney (AUS) | `AEST-10AEDT,M10.1.0/2,M4.1.0/3` |
| Brisbane (no DST) | `AEST-10` |
| Perth | `AWST-8` |
| New Zealand | `NZST-12NZDT,M9.5.0/2,M4.1.0/3` |

## TFT_ROTATION Values (ILI9341 + TFT_eSPI)

| Orientation | Value | Result |
|---|---:|---|
| Default (portrait base) | `0` | Portrait (USB at bottom) |
| Rotate 90 degrees clockwise | `1` | Landscape |
| Rotate 180 degrees | `2` | Portrait inverted (USB at top) |
| Rotate 270 degrees clockwise | `3` | Landscape inverted |

## Audio Configuration Values

| Setting | Recommended | Valid Values | Notes |
|---|---|---|---|
| `ANTHEM_DAC_PIN` | `25` | `25` or `26` | Primary ESP32 DAC output pin |
| `ANTHEM_DAC_PIN_ALT` | `-1` | `-1`, `25`, `26` | Optional mirrored output. Keep `-1` if unused |
| `ANTHEM_GAIN_PCT` | `70` | `10-100` (sheet reference) | Higher gain increases distortion risk depending on amp/speaker |

Project runtime note:

- Current firmware clamps `ANTHEM_GAIN_PCT` to `0..200` internally for safety.

## Polling Frequency (Milliseconds)

| Setting | Recommended | Stable (Lower Load) | Minimum |
|---|---:|---:|---:|
| `POLL_SCOREBOARD_MS` | `15000` | `30000` | `5000` |
| `POLL_GAMEDETAIL_MS` | `8000` | `15000` | `5000` |