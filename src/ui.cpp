#include "ui.h"
#include "palette.h"
#include "assets.h"
#include "config.h"

#include <time.h>

static inline void drawCentered(TFT_eSPI &tft,
                                const String &s,
                                int x,
                                int y,
                                int font,
                                uint16_t fg,
                                uint16_t bg) {
  tft.setTextFont(font);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(s, x, y);
}

static inline void clearScreenWithRotation(TFT_eSPI &tft, uint8_t rotation) {
  // Avoid viewport clipping issues on some CYD panels; just rotate + clear.
  tft.setRotation(rotation);
  tft.resetViewport();
  tft.fillScreen(Palette::BG);
}

struct Layout {
  int16_t w = 0;
  int16_t h = 0;
  int16_t margin = 3;
  int16_t topY = 0;
  int16_t topH = 0;
  int16_t statsY = 0;
  int16_t statsH = 0;
  int16_t statusY = 0;
  int16_t statusH = 0;
  bool landscape = true;
};

static Layout layoutFor(TFT_eSPI &tft) {
  Layout l;
  l.w = tft.width();
  l.h = tft.height();
  l.landscape = (l.w >= l.h);
  l.margin = l.landscape ? 4 : 3;
  const int16_t avail = (int16_t)(l.h - l.margin * 4);
  const float topFrac = l.landscape ? 0.60f : 0.55f;
  const float statsFrac = l.landscape ? 0.22f : 0.24f;
  l.topH = (int16_t)(avail * topFrac);
  l.statsH = (int16_t)(avail * statsFrac);
  l.statusH = (int16_t)(avail - l.topH - l.statsH);
  l.topY = l.margin;
  l.statsY = (int16_t)(l.topY + l.topH + l.margin);
  l.statusY = (int16_t)(l.statsY + l.statsH + l.margin);
  return l;
}

static void drawHeaderBar(TFT_eSPI &tft,
                          int16_t x,
                          int16_t y,
                          int16_t w,
                          int16_t h,
                          const String &label,
                          uint16_t fg,
                          uint16_t bg,
                          bool showDot,
                          uint16_t dotCol) {
  tft.fillRect(x, y, w, h, bg);
  if (showDot) {
    const int16_t dotX = (int16_t)(x + 10);
    const int16_t dotY = (int16_t)(y + h / 2);
    tft.fillCircle(dotX, dotY, 4, dotCol);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.setTextFont(2);
  tft.drawString(label, (int16_t)(x + w / 2), (int16_t)(y + h / 2));
}

static int16_t pickLogoSize(int16_t panelW, int16_t maxLogo, int16_t padding) {
  const int16_t sizes[] = { 96, 64, 56, 48 };
  const int16_t minScoreArea = (panelW >= 300) ? 110 : 90;
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const int16_t s = sizes[i];
    if (s > maxLogo) continue;
    const int16_t scoreAreaW = (int16_t)(panelW - 2 * (s + padding));
    if (scoreAreaW >= minScoreArea) return s;
  }
  return (maxLogo < 48) ? maxLogo : 48;
}

// Forward declaration of drawScoreboardRow
static void drawScoreboardRow(TFT_eSPI &tft,
                              const TeamLine &away,
                              const TeamLine &home,
                              int16_t panelX,
                              int16_t panelW,
                              int16_t rowTop,
                              int16_t logoSize,
                              bool showAbbr,
                              bool showScores,
                              const String &midLabel);

static bool timeLooksValid() {
  // If SNTP has not set the clock, time(nullptr) will be close to 0.
  // Any value above 2020-01-01 is "good enough" for countdowns.
  return time(nullptr) > 1577836800;
}

static String fmtLocalTime(time_t epoch) {
  struct tm lt;
  localtime_r(&epoch, &lt);
  char buf[32];
  // Example: Tue 21 Jan 19:30
  strftime(buf, sizeof(buf), "%a %d %b %H:%M", &lt);
  return String(buf);
}

static String fmtLocalDate(time_t epoch) {
  struct tm lt;
  localtime_r(&epoch, &lt);
  char buf[24];
  // Example: 21 Jan 26
  strftime(buf, sizeof(buf), "%d %b %y", &lt);
  return String(buf);
}

static String fmtLocalClock(time_t epoch) {
  struct tm lt;
  localtime_r(&epoch, &lt);
  char buf[16];
  // Example: 19:30
  strftime(buf, sizeof(buf), "%H:%M", &lt);
  return String(buf);
}

static String fmtCountdown(int64_t seconds) {
  if (seconds < 0) seconds = 0;

  const int64_t days = seconds / 86400;
  seconds %= 86400;
  const int64_t hours = seconds / 3600;
  seconds %= 3600;
  const int64_t mins = seconds / 60;
  const int64_t secs = seconds % 60;

  char buf[32];
  if (days > 0) {
    // Dd HH:MM
    snprintf(buf, sizeof(buf), "%lldd %02lld:%02lld", (long long)days, (long long)hours, (long long)mins);
  } else {
    // HH:MM:SS
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", (long long)hours, (long long)mins, (long long)secs);
  }
  return String(buf);
}

static String elideText(const String &s, size_t maxLen) {
  if (maxLen < 4) return s;
  if (s.length() <= maxLen) return s;
  const int end = (int)maxLen - 3;
  if (end <= 0) return s;
  return s.substring(0, end) + "...";
}

static String periodLabelForStatus(const GameState &g) {
  if (g.periodType == "OT") {
    if (g.period > 4) {
      return String("OT") + String(g.period - 3);
    }
    return "OT";
  }
  if (g.periodType == "SO") return "SO";
  if (g.period > 0) return String("P") + String(g.period);
  return "";
}

static String elideToWidth(TFT_eSPI &tft, const String &s, int maxPx, int font) {
  if (maxPx <= 0) return s;
  if (tft.textWidth(s, font) <= maxPx) return s;
  String out = s;
  while (out.length() > 0 && tft.textWidth(out + "...", font) > maxPx) {
    out.remove(out.length() - 1);
  }
  if (out.length() == 0) return String("...");
  return out + "...";
}

static String fmtStatPair(int away, int home) {
  const String a = (away < 0) ? String("-") : String(away);
  const String h = (home < 0) ? String("-") : String(home);
  if (away < 0 && home < 0) return String("--");
  return a + "-" + h;
}

static String staleRightLabel(const GameState &g, const String &normal) {
  if (!g.wifiConnected) return String("OFFLINE");
  if (g.dataStale) return String("DATA STALE");
  return normal;
}

void Ui::begin(TFT_eSPI &tft, uint8_t rotation) {
  _tft = &tft;
  _rotation = (uint8_t)(rotation & 3);
  _tft->init();
  _tft->invertDisplay(false);
  // Use build-time rotation so portrait/landscape is consistent.
  // For ILI9341: 0/2 = portrait, 1/3 = landscape.
  _tft->setRotation(_rotation);
  _tft->resetViewport();
  _tft->fillScreen(Palette::BG);

  Serial.print("TFT rotation=");
  Serial.print(_rotation);
  Serial.print(" size=");
  Serial.print(_tft->width());
  Serial.print("x");
  Serial.println(_tft->height());

  resetCaches();
}

void Ui::setRotation(uint8_t rotation) {
  if (!_tft) return;
  _rotation = (uint8_t)(rotation & 3);
  _tft->setRotation(_rotation);
  clearScreenWithRotation(*_tft, _rotation);
  _hasLastMode = false;
  resetCaches();
}

void Ui::setBacklight(uint8_t pct) {
  ledcWrite(CYD_BL_PWM_CH, map(pct, 0, 100, 0, 255));
}

void Ui::drawBootSplash(const String &line1, const String &line2) {
  if (!_tft) return;
  clearScreenWithRotation(*_tft, _rotation);
  drawFrame();

  const int16_t W = _tft->width();
  const int16_t H = _tft->height();
  (void)line1;
  const String wifiBanner = line2.length() ? line2 : String("CONNECTING TO WIFI");

  // Prefer SPIFFS splash when present; fallback to vector splash if missing/invalid.
  if (Assets::drawPng(*_tft, "/splash.png", 0, 0)) {
    const int16_t bandH = 18;
    _tft->fillRect(1, (int16_t)(H - bandH - 1), (int16_t)(W - 2), bandH, Palette::PANEL_2);
    drawCentered(*_tft, wifiBanner, W / 2, (int16_t)(H - bandH / 2 - 1), 2, Palette::WHITE, Palette::PANEL_2);
    return;
  }

  // Fallback splash: neutral NHL branding with screen size and status banner.
  drawCentered(*_tft, "NHL SCOREBOARD", W / 2, (int16_t)(H / 2 - 16), 4, Palette::WHITE, Palette::BG);
  const String sizeLine = String(W) + "x" + String(H);
  drawCentered(*_tft, sizeLine, W / 2, (int16_t)(H / 2 + 12), 2, Palette::GREY, Palette::BG);

  const int16_t bandH = 18;
  _tft->fillRect(1, (int16_t)(H - bandH - 1), (int16_t)(W - 2), bandH, Palette::PANEL_2);
  drawCentered(*_tft, wifiBanner, W / 2, (int16_t)(H - bandH / 2 - 1), 2, Palette::WHITE, Palette::PANEL_2);
}

bool Ui::ensureScreen(ScreenMode mode) {
  if (!_hasLastMode || _lastMode != mode) {
    clearScreenWithRotation(*_tft, _rotation);
    drawFrame();
    _lastMode = mode;
    _hasLastMode = true;
    return true;
  }
  return false;
}

void Ui::resetCaches() {
  _liveScore.valid = false;
  _liveStats.valid = false;
  _liveStatus.valid = false;

  _interScore.valid = false;
  _interStats.valid = false;
  _interStatus.valid = false;

  _finalScore.valid = false;
  _finalStats.valid = false;
  _finalStatus.valid = false;

  _preScore.valid = false;
  _preStatus.valid = false;
  _preInfo.valid = false;

  _lastGameKey = "";
  _standingsKey = "";
  _preKey = "";
  _countdownKey = "";
  _countdownValue = "";
  _countdownDate = "";
  _countdownLocation = "";
  _countdownMeta = "";

  _tickerText = "";
  _tickerX = 0;
  _tickerTextW = 0;
  _tickerLastStepMs = 0;
}

void Ui::drawFrame() {
  if (!_tft) return;
  _tft->drawRect(0, 0, _tft->width(), _tft->height(), Palette::FRAME);
}

void Ui::framePanel(int16_t x, int16_t y, int16_t w, int16_t h) {
  _tft->fillRect(x, y, w, h, Palette::PANEL);
  _tft->drawRect(x, y, w, h, Palette::PANEL_2);
}

void Ui::drawTopScorePanel(const GameState &g,
                           const String &label,
                           bool showScores,
                           const String &midLabel) {
  const Layout l = layoutFor(*_tft);
  const int16_t x = l.margin;
  const int16_t y = l.topY;
  const int16_t w = (int16_t)(l.w - l.margin * 2);
  const int16_t h = l.topH;

  framePanel(x, y, w, h);

  const int16_t barH = l.landscape ? 20 : 18;
  const bool showDot = (label == "LIVE");
  drawHeaderBar(*_tft, (int16_t)(x + 1), (int16_t)(y + 1), (int16_t)(w - 2), barH,
                label, Palette::WHITE, Palette::PANEL_2, showDot, Palette::GOLD);

  const int16_t padding = (w >= 300) ? 6 : 5;
  const int16_t maxLogo = (int16_t)(h - barH - 12);
  const int16_t logoSize = pickLogoSize(w, maxLogo, padding);
  const int16_t rowTop = (int16_t)(y + barH + ((h - barH - logoSize) / 2));

  drawScoreboardRow(*_tft,
                    g.home,
                    g.away,
                    x,
                    w,
                    rowTop,
                    logoSize,
                    true,
                    showScores,
                    midLabel);
}

void Ui::drawStatsBand(const GameState &g) {
  const Layout l = layoutFor(*_tft);
  const int16_t x = l.margin;
  const int16_t y = l.statsY;
  const int16_t w = (int16_t)(l.w - l.margin * 2);
  const int16_t h = l.statsH;

  framePanel(x, y, w, h);

  const int16_t colW = (int16_t)(w / 3);
  const int16_t labelY = (int16_t)(y + 6);
  const int16_t valueY = (int16_t)(y + h / 2 + 6);

  _tft->setTextFont(2);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextColor(Palette::GREY, Palette::PANEL);

  _tft->drawString("SOG", (int16_t)(x + colW / 2), labelY);
  _tft->drawString("HITS", (int16_t)(x + colW + colW / 2), labelY);
  _tft->drawString("FO%", (int16_t)(x + 2 * colW + colW / 2), labelY);

  const int16_t valueFont = (h >= 48) ? 4 : 2;
  _tft->setTextFont(valueFont);
  _tft->setTextColor(Palette::WHITE, Palette::PANEL);

  _tft->drawString(fmtStatPair(g.home.sog, g.away.sog), (int16_t)(x + colW / 2), valueY);
  _tft->drawString(fmtStatPair(g.home.hits, g.away.hits), (int16_t)(x + colW + colW / 2), valueY);
  _tft->drawString(fmtStatPair(g.home.foPct, g.away.foPct), (int16_t)(x + 2 * colW + colW / 2), valueY);
}

void Ui::drawStatusBar(const String &left,
                       const String &right,
                       uint16_t dotCol,
                       bool showDot,
                       const String &rightTop) {
  const Layout l = layoutFor(*_tft);
  const int16_t x = l.margin;
  const int16_t y = l.statusY;
  const int16_t w = (int16_t)(l.w - l.margin * 2);
  const int16_t h = l.statusH;

  framePanel(x, y, w, h);

  const int16_t midY = (int16_t)(y + h / 2);
  if (showDot) {
    const int16_t dotX = (int16_t)(x + 10);
    _tft->fillCircle(dotX, midY, 4, dotCol);
  }

  _tft->setTextColor(Palette::WHITE, Palette::PANEL);

  const int16_t valueFont = (h >= 48) ? 4 : 2;
  _tft->setTextFont(valueFont);
  _tft->setTextDatum(ML_DATUM);
  _tft->drawString(left, (int16_t)(x + 20), midY);

  if (rightTop.length()) {
    _tft->setTextFont(1);
    _tft->setTextDatum(TR_DATUM);
    _tft->drawString(rightTop, (int16_t)(x + w - 8), (int16_t)(y + 3));
  }

  _tft->setTextFont(2);
  _tft->setTextDatum(MR_DATUM);
  const int16_t rightY = rightTop.length() ? (int16_t)(midY + 5) : midY;
  _tft->drawString(right, (int16_t)(x + w - 8), rightY);
}

void Ui::drawPregameTicker(const String &text, bool resetPosition) {
  if (!_tft) return;
  const int16_t w = _tft->width();
  const int16_t h = _tft->height();
  const int16_t bandH = 18;
  const int16_t bandY = (int16_t)(h - bandH - 1);
  const int16_t padX = 4;
  const uint32_t nowMs = millis();

  if (resetPosition || _tickerText != text) {
    _tickerText = text;
    _tickerTextW = _tft->textWidth(_tickerText, 2);
    _tickerX = (int16_t)(w - padX);
    _tickerLastStepMs = nowMs;
  }

  const uint32_t stepMs = 40;
  const int16_t pxPerStep = 2;
  if (_tickerLastStepMs == 0) _tickerLastStepMs = nowMs;
  const uint32_t elapsed = nowMs - _tickerLastStepMs;
  if (elapsed >= stepMs) {
    const uint32_t steps = elapsed / stepMs;
    _tickerLastStepMs += steps * stepMs;
    _tickerX = (int16_t)(_tickerX - (int16_t)(steps * pxPerStep));
  }
  if ((int32_t)_tickerX + (int32_t)_tickerTextW < padX) {
    _tickerX = (int16_t)(w - padX);
  }

  _tft->fillRect(1, bandY, (int16_t)(w - 2), bandH, Palette::PANEL_2);
  _tft->setTextDatum(ML_DATUM);
  _tft->setTextFont(2);
  _tft->setTextColor(Palette::WHITE, Palette::PANEL_2);
  _tft->drawString(_tickerText, _tickerX, (int16_t)(bandY + bandH / 2));
}

struct NextGameView {
  String leftAbbr;
  String rightAbbr;
  time_t startEpoch = 0;
  bool isPlayoff = false;
  String venue;
  String city;
};

struct NextGameLayout {
  int16_t logoSize = 0;
  int16_t logoPad = 0;
  int16_t leftLogoX = 0;
  int16_t rightLogoX = 0;
  int16_t rowY = 0;
  int16_t abbrY = 0;
  int16_t seasonY = 0;
  int16_t titleY = 0;
  int16_t countdownY = 0;
  int16_t infoY1 = 0;
  int16_t infoY2 = 0;
  int16_t centerLeft = 0;
  int16_t centerW = 0;
  int16_t countdownBoxH = 0;
  int16_t infoTop = 0;
  int16_t infoH = 0;
  int16_t countdownFont = 0;
};

static NextGameLayout nextGameLayoutFor(const Layout &l) {
  NextGameLayout ng;
  const bool wide = (l.w >= 300);
  ng.logoSize = wide ? 64 : 56;
  ng.logoPad = wide ? 12 : 8;
  ng.countdownFont = wide ? 4 : 2;
  ng.countdownBoxH = wide ? 32 : 20;

  const int16_t titleH = wide ? 40 : 36;
  const int16_t gap1 = wide ? 8 : 6;
  const int16_t rowH = (int16_t)(ng.logoSize + 16);
  const int16_t gap2 = wide ? 8 : 6;
  const int16_t infoH = 32;

  int16_t contentH = (int16_t)(titleH + gap1 + rowH + gap2 + infoH);
  int16_t startY = (int16_t)((l.h - contentH) / 2);
  if (startY < l.margin) startY = l.margin;

  ng.seasonY = (int16_t)(startY + 8);
  ng.titleY = (int16_t)(startY + 30);
  ng.rowY = (int16_t)(startY + titleH + gap1);
  ng.countdownY = (int16_t)(ng.rowY + ng.logoSize / 2 + 2);
  ng.abbrY = (int16_t)(ng.rowY + ng.logoSize + 10);
  ng.infoY1 = (int16_t)(ng.rowY + rowH + gap2 + 6);
  ng.infoY2 = (int16_t)(ng.infoY1 + 16);
  ng.infoTop = (int16_t)(ng.infoY1 - 10);
  ng.infoH = (int16_t)((ng.infoY2 - ng.infoY1) + 20);

  ng.leftLogoX = (int16_t)(l.margin + ng.logoPad);
  ng.rightLogoX = (int16_t)(l.w - l.margin - ng.logoPad - ng.logoSize);

  ng.centerLeft = (int16_t)(ng.leftLogoX + ng.logoSize + ng.logoPad);
  const int16_t centerRight = (int16_t)(ng.rightLogoX - ng.logoPad);
  ng.centerW = (int16_t)(centerRight - ng.centerLeft);
  if (ng.centerW < 0) ng.centerW = 0;

  return ng;
}

static bool buildNextGameView(const GameState &g, const String &focusTeamAbbr, NextGameView &out) {
  if (g.hasNextGame && g.nextOppAbbr.length()) {
    if (g.nextIsHome) {
      out.leftAbbr = focusTeamAbbr;
      out.rightAbbr = g.nextOppAbbr;
    } else {
      out.leftAbbr = g.nextOppAbbr;
      out.rightAbbr = focusTeamAbbr;
    }
    out.startEpoch = g.nextStartEpoch;
    out.isPlayoff = g.nextIsPlayoff;
    out.venue = g.nextVenue;
    out.city = g.nextCity;
    return true;
  }

  if (g.hasGame && g.isPre && g.away.abbr.length() && g.home.abbr.length()) {
    out.leftAbbr = g.home.abbr;
    out.rightAbbr = g.away.abbr;
    out.startEpoch = (g.nextStartEpoch > 0) ? g.nextStartEpoch : g.startEpoch;
    out.isPlayoff = g.isPlayoff;
    out.venue = g.nextVenue;
    out.city = g.nextCity;
    return true;
  }

  return false;
}

enum class NextGamePhase : uint8_t {
  NextGame,
  GameDay,
  PreGame,
};

static NextGamePhase classifyNextGamePhase(const GameState &g, const NextGameView &view) {
  // If time is unavailable, rely on API pre-game signal as a best-effort fallback.
  if (view.startEpoch <= 0 || !timeLooksValid()) {
    return g.isPre ? NextGamePhase::PreGame : NextGamePhase::NextGame;
  }

  const time_t nowEpoch = time(nullptr);
  if (nowEpoch >= view.startEpoch) {
    // Only show GAME DAY/PRE-GAME before puck drop.
    return NextGamePhase::NextGame;
  }

  struct tm nowLocal;
  struct tm gameLocal;
  localtime_r(&nowEpoch, &nowLocal);
  localtime_r(&view.startEpoch, &gameLocal);

  const int64_t secondsToStart = (int64_t)difftime(view.startEpoch, nowEpoch);

  // Special handling for local-time overnight starts (00:00 to 09:59):
  // use a 24h window so "GAME DAY" still appears on the prior evening.
  const bool overnightStart = (gameLocal.tm_hour >= 0 && gameLocal.tm_hour < 10);
  if (overnightStart) {
    if (secondsToStart <= 3 * 60 * 60) return NextGamePhase::PreGame;
    if (secondsToStart <= 24 * 60 * 60) return NextGamePhase::GameDay;
    return NextGamePhase::NextGame;
  }

  const bool sameLocalDay = (nowLocal.tm_year == gameLocal.tm_year) &&
                            (nowLocal.tm_yday == gameLocal.tm_yday);
  if (!sameLocalDay) return NextGamePhase::NextGame;

  return (secondsToStart <= 3 * 60 * 60)
    ? NextGamePhase::PreGame
    : NextGamePhase::GameDay;
}

static String buildFormLine(const GameState &g, const String &focusTeamAbbr, int16_t widthPx, TFT_eSPI &tft) {
  if (!g.standings.hasData || g.standings.teamCount == 0) return String("");

  int idx = -1;
  if (g.standings.focusIndex < g.standings.teamCount) {
    idx = g.standings.focusIndex;
  } else {
    String focus = focusTeamAbbr;
    focus.toUpperCase();
    for (uint8_t i = 0; i < g.standings.teamCount; ++i) {
      String abbr = g.standings.teams[i].abbr;
      abbr.toUpperCase();
      if (abbr == focus) {
        idx = i;
        break;
      }
    }
  }
  if (idx < 0) return String("");

  const ConferenceStandingEntry &e = g.standings.teams[(uint8_t)idx];
  String l10 = String(e.l10Wins) + "-" + String(e.l10Losses) + "-" + String(e.l10OtLosses);
  String strk = String(e.streakCode == '\0' ? '-' : e.streakCode) + String(e.streakCount);
  String line = String("Pts: ") + String(e.points) + " | " + l10 + " | Strk: " + strk;
  return elideToWidth(tft, line, widthPx, 1);
}

static void drawCountdownScreen(TFT_eSPI &tft,
                                const Layout &l,
                                const NextGameView &view,
                                const GameState &g,
                                const String &formLine,
                                bool fullRedraw,
                                const char *title,
                                const char *subtitle,
                                const char *dateLabel,
                                bool showMetaLine,
                                String *countdownCache,
                                String *dateCache,
                                String *locationCache,
                                String *formCache) {
  const NextGameLayout ng = nextGameLayoutFor(l);

  tft.setTextDatum(MC_DATUM);

  if (fullRedraw) {
    if (subtitle && subtitle[0]) {
      tft.setTextColor(Palette::GREY, Palette::BG);
      tft.setTextFont(2);
      tft.drawString(subtitle, (int16_t)(l.w / 2), ng.seasonY);
    }

    tft.setTextColor(Palette::WHITE, Palette::BG);
    tft.setTextFont(4);
    tft.drawString(title ? title : "NEXT GAME", (int16_t)(l.w / 2), ng.titleY);

    if (view.leftAbbr.length()) {
      Assets::drawLogo(tft, view.leftAbbr, ng.leftLogoX, ng.rowY, ng.logoSize);
    }
    if (view.rightAbbr.length()) {
      Assets::drawLogo(tft, view.rightAbbr, ng.rightLogoX, ng.rowY, ng.logoSize);
    }

    tft.setTextColor(Palette::GREY, Palette::BG);
    tft.setTextFont(2);
    if (view.leftAbbr.length()) {
      tft.drawString(view.leftAbbr, (int16_t)(ng.leftLogoX + ng.logoSize / 2), ng.abbrY);
    }
    if (view.rightAbbr.length()) {
      tft.drawString(view.rightAbbr, (int16_t)(ng.rightLogoX + ng.logoSize / 2), ng.abbrY);
    }
  }

  const String staleLabel = !g.wifiConnected ? String("OFFLINE") : (g.dataStale ? String("DATA STALE") : String(""));
  const int16_t badgeW = (l.w >= 300) ? 110 : 92;
  const int16_t badgeH = 16;
  const int16_t badgeX = (int16_t)(l.w - l.margin - badgeW);
  const int16_t badgeY = (int16_t)(l.margin + 2);
  tft.fillRect(badgeX, badgeY, badgeW, badgeH, Palette::BG);
  if (staleLabel.length()) {
    tft.setTextDatum(MR_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(Palette::GREY, Palette::BG);
    tft.drawString(staleLabel, (int16_t)(l.w - l.margin - 2), (int16_t)(badgeY + badgeH / 2));
    tft.setTextDatum(MC_DATUM);
  }

  String countdown = "--:--:--";
  if (view.startEpoch > 0 && timeLooksValid()) {
    const int64_t seconds = (int64_t)difftime(view.startEpoch, time(nullptr));
    countdown = fmtCountdown(seconds);
  }

  String dateLine = String(dateLabel && dateLabel[0] ? dateLabel : "GAME DATE");
  dateLine += ": ";
  if (view.startEpoch > 0) {
    dateLine += fmtLocalDate(view.startEpoch);
    dateLine += " ";
    dateLine += fmtLocalClock(view.startEpoch);
  } else {
    dateLine += "TBA";
  }
  const size_t maxDateLen = (l.w >= 300) ? 28 : 24;
  dateLine = elideText(dateLine, maxDateLen);

  String location;
  if (view.venue.length()) {
    location = view.venue;
    if (view.city.length()) {
      location += " - ";
      location += view.city;
    }
  } else if (view.city.length()) {
    location = view.city;
  } else {
    location = "TBA";
  }
  String locationLine = String("LOCATION: ") + location;
  const size_t maxLocLen = (l.w >= 300) ? 28 : 24;
  locationLine = elideText(locationLine, maxLocLen);

  bool infoChanged = fullRedraw;
  if (dateCache && *dateCache != dateLine) infoChanged = true;
  if (locationCache && *locationCache != locationLine) infoChanged = true;
  if (formCache && *formCache != formLine) infoChanged = true;

  if (infoChanged) {
    tft.fillRect(l.margin, ng.infoTop, (int16_t)(l.w - l.margin * 2), ng.infoH, Palette::BG);
    tft.setTextFont(2);
    tft.setTextColor(Palette::WHITE, Palette::BG);
    tft.drawString(dateLine, (int16_t)(l.w / 2), ng.infoY1);
    tft.setTextColor(Palette::GREY, Palette::BG);
    tft.drawString(locationLine, (int16_t)(l.w / 2), ng.infoY2);

    const int16_t metaY = (int16_t)(l.h - l.margin - 6);
    const int16_t metaH = 12;
    const int16_t metaTop = (int16_t)(metaY - metaH / 2);
    tft.fillRect(l.margin, metaTop, (int16_t)(l.w - l.margin * 2), metaH, Palette::BG);
    if (showMetaLine && formLine.length()) {
      tft.setTextFont(1);
      tft.setTextColor(Palette::GREY, Palette::BG);
      tft.drawString(formLine, (int16_t)(l.w / 2), metaY);
    }
    if (dateCache) *dateCache = dateLine;
    if (locationCache) *locationCache = locationLine;
    if (formCache) *formCache = showMetaLine ? formLine : String("");
  }

  if (!countdownCache || *countdownCache != countdown) {
    if (ng.centerW > 0) {
      tft.fillRect(ng.centerLeft,
                   (int16_t)(ng.countdownY - ng.countdownBoxH / 2),
                   ng.centerW,
                   ng.countdownBoxH,
                   Palette::BG);
    }
    tft.setTextColor(Palette::WHITE, Palette::BG);
    int16_t countdownFont = ng.countdownFont;
    if (countdown.length() > 8 && countdownFont > 2) {
      countdownFont = 2;
    }
    tft.setTextFont(countdownFont);
    tft.drawString(countdown, (int16_t)(l.w / 2), ng.countdownY);
    if (countdownCache) *countdownCache = countdown;
  }
}

// -----------------------------------------------------------------------------
// NO GAME
// -----------------------------------------------------------------------------

void Ui::drawNoGame(const GameState &g, const String &focusTeamAbbr) {
  const bool modeChanged = ensureScreen(ScreenMode::NO_GAME);
  NextGameView view;
  const bool hasNext = buildNextGameView(g, focusTeamAbbr, view);
  const NextGamePhase phase = hasNext ? classifyNextGamePhase(g, view) : NextGamePhase::NextGame;
  const char *phaseKey = "NEXT";
  if (phase == NextGamePhase::GameDay) phaseKey = "GAMEDAY";
  else if (phase == NextGamePhase::PreGame) phaseKey = "PRE";
  const char *compKey = hasNext ? (view.isPlayoff ? "PO" : "RS") : "RS";
  const String key = hasNext
    ? (view.leftAbbr + "|" + view.rightAbbr + "|" + String((long)view.startEpoch) + "|" + phaseKey + "|" + compKey)
    : String("NONE");
  bool fullRedraw = modeChanged;
  if (key != _noGameKey) {
    _noGameKey = key;
    fullRedraw = true;
  }
  if (fullRedraw && !modeChanged) {
    clearScreenWithRotation(*_tft, _rotation);
    drawFrame();
  }

  const Layout l = layoutFor(*_tft);

  if (hasNext) {
    if (_countdownKey != key) {
      _countdownKey = key;
      _countdownValue = "";
      _countdownDate = "";
      _countdownLocation = "";
      _countdownMeta = "";
      fullRedraw = true;
    }
    const char *title = "NEXT GAME";
    const char *subtitle = view.isPlayoff ? "PLAYOFF" : "SEASON";
    const char *dateLabel = "GAME DATE";
    if (phase == NextGamePhase::GameDay) {
      title = "GAME DAY";
      dateLabel = "PUCK DROP";
    } else if (phase == NextGamePhase::PreGame) {
      title = "PRE-GAME";
      dateLabel = "PUCK DROP";
    }
    const bool showPregameTicker = (phase == NextGamePhase::PreGame) && g.pregameTicker.length();
    const bool showMetaLine = !showPregameTicker;
    String formLine = buildFormLine(g, focusTeamAbbr, (int16_t)(l.w - l.margin * 2 - 8), *_tft);
    drawCountdownScreen(*_tft, l, view, g, formLine, fullRedraw, title, subtitle, dateLabel, showMetaLine,
                        &_countdownValue, &_countdownDate, &_countdownLocation, &_countdownMeta);
    if (showPregameTicker) {
      drawPregameTicker(g.pregameTicker, fullRedraw);
    } else if (_tickerText.length()) {
      _tickerText = "";
      _tickerX = 0;
      _tickerTextW = 0;
      _tickerLastStepMs = 0;
      const int16_t bandH = 18;
      const int16_t bandY = (int16_t)(l.h - bandH - 1);
      _tft->fillRect(1, bandY, (int16_t)(l.w - 2), bandH, Palette::BG);
    }
  } else if (fullRedraw) {
    const int16_t panelX2 = l.margin;
    const int16_t panelW2 = (int16_t)(l.w - l.margin * 2);
    framePanel(panelX2, l.topY, panelW2, l.topH);
    drawCentered(*_tft, "NO GAME TODAY", l.w / 2, (int16_t)(l.topY + l.topH / 2 - 10), 4, Palette::WHITE, Palette::PANEL);
    drawCentered(*_tft, "CHECKING SCHEDULE", l.w / 2, (int16_t)(l.topY + l.topH / 2 + 18), 2, Palette::GREY, Palette::PANEL);
    framePanel(panelX2, l.statsY, panelW2, l.statsH);
    drawCentered(*_tft, "CONNECTING...", l.w / 2, (int16_t)(l.statsY + l.statsH / 2), 2, Palette::WHITE, Palette::PANEL);
    framePanel(panelX2, l.statusY, panelW2, l.statusH);
    _tickerText = "";
    _tickerX = 0;
    _tickerTextW = 0;
    _tickerLastStepMs = 0;
  }
}

// -----------------------------------------------------------------------------
// LAST GAME RECAP
// -----------------------------------------------------------------------------

static String formatScorer(const ScorerEntry &entry) {
  if (entry.name.isEmpty()) return String("-");
  if (entry.goals > 1) {
    return entry.name + " (" + String(entry.goals) + ")";
  }
  return entry.name;
}

static String buildPeriodLine(const LastGameRecap &recap, int16_t w) {
  if (!recap.periodCount) return String("PERIODS: TBA");
  String line;
  for (uint8_t i = 0; i < recap.periodCount; ++i) {
    if (i) line += "  ";
    line += recap.periods[i].label;
    line += " ";
    line += String(recap.periods[i].home);
    line += "-";
    line += String(recap.periods[i].away);
  }
  const size_t maxLen = (w >= 300) ? 32 : 26;
  return elideText(line, maxLen);
}

static String buildStandingsKey(const GameState &g) {
  String key = g.standings.hasData ? String("HAS|") : String("NONE|");
  key += g.standings.conferenceAbbrev;
  key += "|";
  key += g.standings.conferenceName;
  key += "|";
  key += String(g.standings.teamCount);
  key += "|";
  key += String(g.standings.focusIndex);
  key += "|";
  key += String((int)g.wifiConnected);
  key += "|";
  key += String((int)g.dataStale);
  for (uint8_t i = 0; i < g.standings.teamCount; ++i) {
    const ConferenceStandingEntry &e = g.standings.teams[i];
    key += "|";
    key += String(e.rank);
    key += ":";
    key += e.divisionAbbrev;
    key += ":";
    key += String(e.divisionRank);
    key += ":";
    key += e.abbr;
    key += ":";
    key += String(e.points);
    key += ":";
    key += String(e.gamesPlayed);
  }
  return key;
}

void Ui::drawLastGame(const GameState &g) {
  const bool modeChanged = ensureScreen(ScreenMode::LAST_GAME);
  const String key = g.last.hasGame ? g.last.gameId : String("NONE");
  bool fullRedraw = modeChanged || key != _lastGameKey;
  if (fullRedraw && !modeChanged) {
    clearScreenWithRotation(*_tft, _rotation);
    drawFrame();
  }
  _lastGameKey = key;

  const Layout l = layoutFor(*_tft);
  const int16_t x = l.margin;
  const int16_t w = (int16_t)(l.w - l.margin * 2);

  // Top score panel
  framePanel(x, l.topY, w, l.topH);
  const int16_t barH = l.landscape ? 20 : 18;
  drawHeaderBar(*_tft, (int16_t)(x + 1), (int16_t)(l.topY + 1), (int16_t)(w - 2), barH,
                "LAST GAME", Palette::WHITE, Palette::PANEL_2, false, Palette::GOLD);

  if (!g.last.hasGame) {
    drawCentered(*_tft, "NO RECENT GAME", l.w / 2, (int16_t)(l.topY + l.topH / 2), 4, Palette::WHITE, Palette::PANEL);
    return;
  }

  const int16_t padding = (w >= 300) ? 6 : 5;
  const int16_t maxLogo = (int16_t)(l.topH - barH - 12);
  const int16_t logoSize = pickLogoSize(w, maxLogo, padding);
  const int16_t rowTop = (int16_t)(l.topY + barH + ((l.topH - barH - logoSize) / 2));

  // drawScoreboardRow expects left=away; pass home first so home is on the left.
  drawScoreboardRow(*_tft,
                    g.last.home,
                    g.last.away,
                    x,
                    w,
                    rowTop,
                    logoSize,
                    true,
                    true,
                    "-");

  // Scorers panel
  framePanel(x, l.statsY, w, l.statsH);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextFont(2);
  _tft->setTextColor(Palette::GREY, Palette::PANEL);
  _tft->drawString("SCORERS", l.w / 2, (int16_t)(l.statsY + 8));

  const int16_t colPad = 10;
  const int16_t leftX = (int16_t)(x + colPad);
  const int16_t rightX = (int16_t)(x + w - colPad);
  const int16_t listTop = (int16_t)(l.statsY + 18);
  const int16_t listBottom = (int16_t)(l.statsY + l.statsH - 6);
  const int16_t listH = (int16_t)(listBottom - listTop);
  const uint8_t maxLines = (listH >= 50) ? 3 : 2;
  const int16_t lineH = (maxLines > 0) ? (int16_t)(listH / maxLines) : 16;
  const int16_t startY = (int16_t)(listTop + lineH / 2);
  const int16_t colW = (int16_t)(w / 2 - colPad * 2);
  const int scorerFont = (lineH < 16) ? 1 : 2;

  _tft->setTextColor(Palette::WHITE, Palette::PANEL);
  _tft->setTextFont(scorerFont);

  for (uint8_t i = 0; i < maxLines; ++i) {
    String leftLine = "-";
    if (i < g.last.homeScorerCount) leftLine = formatScorer(g.last.homeScorers[i]);
    leftLine = elideToWidth(*_tft, leftLine, colW, scorerFont);
    String rightLine = "-";
    if (i < g.last.awayScorerCount) rightLine = formatScorer(g.last.awayScorers[i]);
    rightLine = elideToWidth(*_tft, rightLine, colW, scorerFont);
    const int16_t y = (int16_t)(startY + i * lineH);
    _tft->setTextDatum(ML_DATUM);
    _tft->drawString(leftLine, leftX, y);
    _tft->setTextDatum(MR_DATUM);
    _tft->drawString(rightLine, rightX, y);
  }
  _tft->setTextDatum(MC_DATUM);

  // Period stats panel
  framePanel(x, l.statusY, w, l.statusH);
  String periodLine = buildPeriodLine(g.last, l.w);
  _tft->setTextColor(Palette::WHITE, Palette::PANEL);
  _tft->setTextFont(2);
  _tft->drawString(periodLine, l.w / 2, (int16_t)(l.statusY + l.statusH / 2));
}

struct DivisionBucket {
  String abbrev;
  String name;
  uint8_t indices[8] = {0};
  uint8_t count = 0;
};

static String uppercaseCopy(const String &in) {
  String out = in;
  out.toUpperCase();
  return out;
}

static String fallbackDivisionName(const String &abbrUpper) {
  if (abbrUpper == "A") return "ATLANTIC";
  if (abbrUpper == "M") return "METROPOLITAN";
  if (abbrUpper == "C") return "CENTRAL";
  if (abbrUpper == "P") return "PACIFIC";
  return abbrUpper;
}

static void expectedDivisionsForConference(const String &conferenceAbbrev,
                                           String &leftDiv,
                                           String &rightDiv) {
  leftDiv = "";
  rightDiv = "";
  const String conf = uppercaseCopy(conferenceAbbrev);

  // Eastern conference split:
  // left = Atlantic (A), right = Metropolitan (M)
  if (conf == "E" || conf == "EASTERN") {
    leftDiv = "A";
    rightDiv = "M";
    return;
  }

  // Western conference split:
  // left = Central (C), right = Pacific (P)
  if (conf == "W" || conf == "WESTERN") {
    leftDiv = "C";
    rightDiv = "P";
  }
}

static uint16_t standingsDivisionSortKey(const ConferenceStandingEntry &e) {
  const uint16_t divRank = e.divisionRank ? e.divisionRank : (uint16_t)(200 + e.rank);
  const uint16_t confRank = e.rank ? e.rank : 250;
  return (uint16_t)(divRank * 16 + confRank);
}

static void sortDivisionBucket(const GameState &g, DivisionBucket &bucket) {
  for (uint8_t i = 0; i < bucket.count; ++i) {
    uint8_t best = i;
    for (uint8_t j = (uint8_t)(i + 1); j < bucket.count; ++j) {
      const ConferenceStandingEntry &a = g.standings.teams[bucket.indices[best]];
      const ConferenceStandingEntry &b = g.standings.teams[bucket.indices[j]];
      if (standingsDivisionSortKey(b) < standingsDivisionSortKey(a)) {
        best = j;
      }
    }
    if (best != i) {
      const uint8_t tmp = bucket.indices[i];
      bucket.indices[i] = bucket.indices[best];
      bucket.indices[best] = tmp;
    }
  }
}

void Ui::drawStandings(const GameState &g, const String &focusTeamAbbr) {
  const bool modeChanged = ensureScreen(ScreenMode::STANDINGS);
  const String key = buildStandingsKey(g);
  bool fullRedraw = modeChanged || (_standingsKey != key);
  if (fullRedraw && !modeChanged) {
    clearScreenWithRotation(*_tft, _rotation);
    drawFrame();
  }
  _standingsKey = key;
  if (!fullRedraw) return;

  const Layout l = layoutFor(*_tft);
  const int16_t x = l.margin;
  const int16_t w = (int16_t)(l.w - l.margin * 2);

  framePanel(x, l.topY, w, l.topH);
  framePanel(x, l.statsY, w, l.statsH);
  framePanel(x, l.statusY, w, l.statusH);

  const int16_t barH = l.landscape ? 20 : 18;
  String header = "CONFERENCE STANDINGS";
  if (g.standings.conferenceName.length()) {
    header = g.standings.conferenceName + " CONFERENCE";
  } else if (g.standings.conferenceAbbrev.length()) {
    header = g.standings.conferenceAbbrev + " CONFERENCE";
  }
  header = elideToWidth(*_tft, header, (int)(w - 16), 2);
  drawHeaderBar(*_tft, (int16_t)(x + 1), (int16_t)(l.topY + 1), (int16_t)(w - 2), barH,
                header, Palette::WHITE, Palette::PANEL_2, false, Palette::GOLD);

  const String staleLabel = staleRightLabel(g, "PTS/GP");
  _tft->setTextDatum(ML_DATUM);
  _tft->setTextFont(2);
  _tft->setTextColor(Palette::GREY, Palette::PANEL);
  _tft->drawString("DIV RK TEAM", (int16_t)(x + 8), (int16_t)(l.statusY + l.statusH / 2));
  _tft->setTextDatum(MR_DATUM);
  _tft->drawString(staleLabel, (int16_t)(x + w - 8), (int16_t)(l.statusY + l.statusH / 2));

  if (!g.standings.hasData || g.standings.teamCount == 0) {
    _tft->setTextDatum(MC_DATUM);
    _tft->setTextColor(Palette::WHITE, Palette::PANEL);
    _tft->setTextFont(2);
    _tft->drawString("STANDINGS UNAVAILABLE", l.w / 2, (int16_t)(l.topY + l.topH / 2 + 8));
    _tft->setTextColor(Palette::GREY, Palette::PANEL);
    _tft->drawString("CHECKING NHL API", l.w / 2, (int16_t)(l.statsY + l.statsH / 2));
    return;
  }

  uint8_t focusIdx = g.standings.focusIndex;
  if (focusIdx >= g.standings.teamCount) {
    String focus = focusTeamAbbr;
    focus.toUpperCase();
    for (uint8_t i = 0; i < g.standings.teamCount; ++i) {
      String abbr = g.standings.teams[i].abbr;
      abbr.toUpperCase();
      if (abbr == focus) {
        focusIdx = i;
        break;
      }
    }
  }

  String expectedLeftDiv;
  String expectedRightDiv;
  expectedDivisionsForConference(g.standings.conferenceAbbrev, expectedLeftDiv, expectedRightDiv);

  DivisionBucket leftBucket;
  DivisionBucket rightBucket;
  leftBucket.abbrev = expectedLeftDiv;
  rightBucket.abbrev = expectedRightDiv;

  for (uint8_t i = 0; i < g.standings.teamCount; ++i) {
    const ConferenceStandingEntry &e = g.standings.teams[i];
    String divAbbr = uppercaseCopy(e.divisionAbbrev);
    if (divAbbr.isEmpty()) {
      if (i < 8) {
        divAbbr = expectedLeftDiv.length() ? expectedLeftDiv : String("A");
      } else {
        divAbbr = expectedRightDiv.length() ? expectedRightDiv : String("M");
      }
    }

    DivisionBucket *target = nullptr;
    if (!leftBucket.abbrev.isEmpty() && divAbbr == leftBucket.abbrev) {
      target = &leftBucket;
    } else if (!rightBucket.abbrev.isEmpty() && divAbbr == rightBucket.abbrev) {
      target = &rightBucket;
    } else if (leftBucket.count <= rightBucket.count) {
      if (leftBucket.abbrev.isEmpty()) leftBucket.abbrev = divAbbr;
      target = &leftBucket;
    } else {
      if (rightBucket.abbrev.isEmpty()) rightBucket.abbrev = divAbbr;
      target = &rightBucket;
    }

    if (target && target->count < 8) {
      if (target->name.isEmpty() && e.divisionName.length()) {
        target->name = uppercaseCopy(e.divisionName);
      }
      target->indices[target->count++] = i;
    }
  }

  sortDivisionBucket(g, leftBucket);
  sortDivisionBucket(g, rightBucket);

  if (leftBucket.name.isEmpty()) leftBucket.name = fallbackDivisionName(leftBucket.abbrev);
  if (rightBucket.name.isEmpty()) rightBucket.name = fallbackDivisionName(rightBucket.abbrev);
  if (leftBucket.name.isEmpty()) leftBucket.name = "DIVISION 1";
  if (rightBucket.name.isEmpty()) rightBucket.name = "DIVISION 2";

  const uint8_t rowsPerCol = 8;
  const int16_t tableTop = (int16_t)(l.topY + barH + 4);
  const int16_t tableBottom = (int16_t)(l.statsY + l.statsH - 4);
  const int16_t colGap = 6;
  const int16_t colW = (int16_t)((w - colGap - 6) / 2);
  const int16_t leftColX = (int16_t)(x + 3);
  const int16_t rightColX = (int16_t)(leftColX + colW + colGap);
  const int16_t dividerX = (int16_t)(x + w / 2);
  const int16_t divisionHeaderH = 14;
  const int16_t rowsTop = (int16_t)(tableTop + divisionHeaderH + 2);
  int16_t rowH = (int16_t)((tableBottom - rowsTop) / rowsPerCol);
  if (rowH < 10) rowH = 10;

  _tft->drawLine(dividerX, tableTop, dividerX, tableBottom, Palette::PANEL_2);

  _tft->fillRect(leftColX, tableTop, colW, divisionHeaderH, Palette::PANEL_2);
  _tft->fillRect(rightColX, tableTop, colW, divisionHeaderH, Palette::PANEL_2);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextFont(2);
  _tft->setTextColor(Palette::WHITE, Palette::PANEL_2);
  _tft->drawString(elideToWidth(*_tft, leftBucket.name, (int)(colW - 8), 2),
                   (int16_t)(leftColX + colW / 2),
                   (int16_t)(tableTop + divisionHeaderH / 2));
  _tft->drawString(elideToWidth(*_tft, rightBucket.name, (int)(colW - 8), 2),
                   (int16_t)(rightColX + colW / 2),
                   (int16_t)(tableTop + divisionHeaderH / 2));

  auto drawDivisionRow = [&](const DivisionBucket &bucket, uint8_t row, int16_t cellX) {
    const int16_t yTop = (int16_t)(rowsTop + row * rowH);
    const int16_t yMid = (int16_t)(yTop + rowH / 2);

    int entryIndex = -1;
    if (row < bucket.count) {
      entryIndex = bucket.indices[row];
    }

    const bool isFocus = (entryIndex >= 0 && (uint8_t)entryIndex == focusIdx);
    const uint16_t bg = isFocus ? Palette::LEAFS_BLUE : Palette::PANEL;
    _tft->fillRect(cellX, yTop, colW, (int16_t)(rowH - 1), bg);

    if (entryIndex < 0) return;

    const ConferenceStandingEntry &e = g.standings.teams[(uint8_t)entryIndex];
    const uint8_t divRank = e.divisionRank ? e.divisionRank : (e.rank ? e.rank : (uint8_t)(row + 1));
    String leftText = String(divRank) + " " + e.abbr;
    leftText = elideToWidth(*_tft, leftText, (int)(colW - 32), 2);

    _tft->setTextFont(2);
    _tft->setTextColor(Palette::WHITE, bg);
    _tft->setTextDatum(ML_DATUM);
    _tft->drawString(leftText, (int16_t)(cellX + 2), yMid);
    const String rightText = String(e.points) + "/" + String(e.gamesPlayed);
    _tft->setTextDatum(MR_DATUM);
    _tft->drawString(rightText, (int16_t)(cellX + colW - 2), yMid);
  };

  for (uint8_t row = 0; row < rowsPerCol; ++row) {
    drawDivisionRow(leftBucket, row, leftColX);
    drawDivisionRow(rightBucket, row, rightColX);
  }
}

// -----------------------------------------------------------------------------
// GAME SCREENS
// -----------------------------------------------------------------------------

void Ui::drawPregame(const GameState &g, const String &focusTeamAbbr) {
  drawNoGame(g, focusTeamAbbr);
}

void Ui::drawLive(const GameState &g) {
  const bool modeChanged = ensureScreen(ScreenMode::LIVE);

  bool scoreChanged = modeChanged || !_liveScore.valid
    || _liveScore.homeAbbr != g.home.abbr
    || _liveScore.awayAbbr != g.away.abbr
    || _liveScore.homeScore != g.home.score
    || _liveScore.awayScore != g.away.score;
  if (scoreChanged) {
    drawTopScorePanel(g, "LIVE", true, "-");
    _liveScore.valid = true;
    _liveScore.homeAbbr = g.home.abbr;
    _liveScore.awayAbbr = g.away.abbr;
    _liveScore.homeScore = g.home.score;
    _liveScore.awayScore = g.away.score;
  }

  bool statsChanged = modeChanged || !_liveStats.valid
    || _liveStats.homeSog != g.home.sog
    || _liveStats.awaySog != g.away.sog
    || _liveStats.homeHits != g.home.hits
    || _liveStats.awayHits != g.away.hits
    || _liveStats.homeFo != g.home.foPct
    || _liveStats.awayFo != g.away.foPct;
  if (statsChanged) {
    drawStatsBand(g);
    _liveStats.valid = true;
    _liveStats.homeSog = g.home.sog;
    _liveStats.awaySog = g.away.sog;
    _liveStats.homeHits = g.home.hits;
    _liveStats.awayHits = g.away.hits;
    _liveStats.homeFo = g.home.foPct;
    _liveStats.awayFo = g.away.foPct;
  }

  String clockLine = g.clock.length() ? g.clock : String("IN PLAY");
  const String periodLabel = periodLabelForStatus(g);
  if (periodLabel.length()) {
    clockLine += "  ";
    clockLine += periodLabel;
  }
  String strength = g.strengthLabel.length() ? g.strengthLabel : String("EVEN STRENGTH");
  strength = staleRightLabel(g, strength);
  const String rightTop = g.hasPenaltyCountdown ? g.penaltyCountdown : String("");

  const bool statusChanged = modeChanged || !_liveStatus.valid
    || _liveStatus.left != clockLine
    || _liveStatus.right != strength
    || _liveStatus.rightTop != rightTop
    || _liveStatus.showDot != true
    || _liveStatus.dotCol != Palette::STATUS_PK;
  if (statusChanged) {
    drawStatusBar(clockLine, strength, Palette::STATUS_PK, true, rightTop);
    _liveStatus.valid = true;
    _liveStatus.left = clockLine;
    _liveStatus.right = strength;
    _liveStatus.rightTop = rightTop;
    _liveStatus.dotCol = Palette::STATUS_PK;
    _liveStatus.showDot = true;
  }
}

void Ui::drawIntermission(const GameState &g) {
  const bool modeChanged = ensureScreen(ScreenMode::INTERMISSION);

  bool scoreChanged = modeChanged || !_interScore.valid
    || _interScore.homeAbbr != g.home.abbr
    || _interScore.awayAbbr != g.away.abbr
    || _interScore.homeScore != g.home.score
    || _interScore.awayScore != g.away.score;
  if (scoreChanged) {
    drawTopScorePanel(g, "INTERMISSION", true, "-");
    _interScore.valid = true;
    _interScore.homeAbbr = g.home.abbr;
    _interScore.awayAbbr = g.away.abbr;
    _interScore.homeScore = g.home.score;
    _interScore.awayScore = g.away.score;
  }

  bool statsChanged = modeChanged || !_interStats.valid
    || _interStats.homeSog != g.home.sog
    || _interStats.awaySog != g.away.sog
    || _interStats.homeHits != g.home.hits
    || _interStats.awayHits != g.away.hits
    || _interStats.homeFo != g.home.foPct
    || _interStats.awayFo != g.away.foPct;
  if (statsChanged) {
    drawStatsBand(g);
    _interStats.valid = true;
    _interStats.homeSog = g.home.sog;
    _interStats.awaySog = g.away.sog;
    _interStats.homeHits = g.home.hits;
    _interStats.awayHits = g.away.hits;
    _interStats.homeFo = g.home.foPct;
    _interStats.awayFo = g.away.foPct;
  }

  String left = "INTERMISSION";
  const String periodLabel = periodLabelForStatus(g);
  if (periodLabel.length()) {
    left = "END " + periodLabel;
  }
  String right = staleRightLabel(g, "BREAK");

  const bool statusChanged = modeChanged || !_interStatus.valid
    || _interStatus.left != left
    || _interStatus.right != right
    || _interStatus.rightTop.length()
    || _interStatus.showDot != false
    || _interStatus.dotCol != Palette::STATUS_EVEN;
  if (statusChanged) {
    drawStatusBar(left, right, Palette::STATUS_EVEN, false, String(""));
    _interStatus.valid = true;
    _interStatus.left = left;
    _interStatus.right = right;
    _interStatus.rightTop = "";
    _interStatus.dotCol = Palette::STATUS_EVEN;
    _interStatus.showDot = false;
  }
}

void Ui::drawFinal(const GameState &g) {
  const bool modeChanged = ensureScreen(ScreenMode::FINAL);

  bool scoreChanged = modeChanged || !_finalScore.valid
    || _finalScore.homeAbbr != g.home.abbr
    || _finalScore.awayAbbr != g.away.abbr
    || _finalScore.homeScore != g.home.score
    || _finalScore.awayScore != g.away.score;
  if (scoreChanged) {
    drawTopScorePanel(g, "FINAL", true, "-");
    _finalScore.valid = true;
    _finalScore.homeAbbr = g.home.abbr;
    _finalScore.awayAbbr = g.away.abbr;
    _finalScore.homeScore = g.home.score;
    _finalScore.awayScore = g.away.score;
  }

  bool statsChanged = modeChanged || !_finalStats.valid
    || _finalStats.homeSog != g.home.sog
    || _finalStats.awaySog != g.away.sog
    || _finalStats.homeHits != g.home.hits
    || _finalStats.awayHits != g.away.hits
    || _finalStats.homeFo != g.home.foPct
    || _finalStats.awayFo != g.away.foPct;
  if (statsChanged) {
    drawStatsBand(g);
    _finalStats.valid = true;
    _finalStats.homeSog = g.home.sog;
    _finalStats.awaySog = g.away.sog;
    _finalStats.homeHits = g.home.hits;
    _finalStats.awayHits = g.away.hits;
    _finalStats.homeFo = g.home.foPct;
    _finalStats.awayFo = g.away.foPct;
  }

  String right = staleRightLabel(g, "FULL TIME");
  const bool statusChanged = modeChanged || !_finalStatus.valid
    || _finalStatus.left != "FINAL"
    || _finalStatus.right != right
    || _finalStatus.rightTop.length()
    || _finalStatus.showDot != false
    || _finalStatus.dotCol != Palette::STATUS_EVEN;
  if (statusChanged) {
    drawStatusBar("FINAL", right, Palette::STATUS_EVEN, false, String(""));
    _finalStatus.valid = true;
    _finalStatus.left = "FINAL";
    _finalStatus.right = right;
    _finalStatus.rightTop = "";
    _finalStatus.dotCol = Palette::STATUS_EVEN;
    _finalStatus.showDot = false;
  }

  // Small dismiss hint for the held FINAL screen.
  const int16_t noteY = (int16_t)(_tft->height() - 7);
  _tft->fillRect(4, (int16_t)(noteY - 5), (int16_t)(_tft->width() - 8), 10, Palette::BG);
  _tft->setTextDatum(MC_DATUM);
  _tft->setTextFont(1);
  _tft->setTextColor(Palette::GREY, Palette::BG);
  _tft->drawString("CLICK BOOT TO DISMISS", (int16_t)(_tft->width() / 2), noteY);
}

void Ui::drawGoal(const GameState &g) {
  ensureScreen(ScreenMode::GOAL);
  const uint16_t bg = g.leafsJustScored ? Palette::LEAFS_BLUE : Palette::PANEL_2;
  _tft->fillScreen(bg);

  drawCentered(*_tft, "GOAL!", _tft->width() / 2, 54, 4, Palette::WHITE, bg);

  if (g.goalTeamAbbr.length()) {
    const int16_t logoSize = 96;
    const int16_t logoX = (int16_t)(_tft->width() / 2 - logoSize / 2);
    const int16_t logoY = 78;
    Assets::drawLogo(*_tft, g.goalTeamAbbr, logoX, logoY, logoSize);
  }

  const int16_t textWidth = (int16_t)(_tft->width() - 16);
  if (g.goalScorer.length()) {
    String scorerLine = elideToWidth(*_tft, g.goalScorer, textWidth, 2);
    drawCentered(*_tft, scorerLine, _tft->width() / 2, 186, 2, Palette::WHITE, bg);
  }

  if (g.goalText.length()) {
    String detailLine = elideToWidth(*_tft, g.goalText, textWidth, 2);
    drawCentered(*_tft, detailLine, _tft->width() / 2, 206, 2, Palette::WHITE, bg);
  }
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void drawScoreboardRow(TFT_eSPI &tft,
                              const TeamLine &away,
                              const TeamLine &home,
                              int16_t panelX,
                              int16_t panelW,
                              int16_t rowTop,
                              int16_t logoSize,
                              bool showAbbr,
                              bool showScores,
                              const String &midLabel) {
  const int16_t padding = (panelW >= 300) ? 6 : 5;
  const int16_t logoY = rowTop;
  const int16_t logoYMid = (int16_t)(logoY + logoSize / 2);

  const int16_t leftLogoX = (int16_t)(panelX + padding);
  const int16_t rightLogoX = (int16_t)(panelX + panelW - padding - logoSize);

  const int16_t scoreAreaX = (int16_t)(leftLogoX + logoSize + padding);
  const int16_t scoreAreaW = (int16_t)(rightLogoX - padding - scoreAreaX);

  const int16_t leftScoreX = (int16_t)(scoreAreaX + scoreAreaW / 4);
  const int16_t dashX = (int16_t)(scoreAreaX + scoreAreaW / 2);
  const int16_t rightScoreX = (int16_t)(scoreAreaX + (scoreAreaW * 3) / 4);
  const int16_t scoreY = (int16_t)(logoYMid + 2);

  const bool bigScores = (scoreAreaW >= 120);
  const int16_t scoreFont = bigScores ? 6 : 4;
  const int16_t scoreBoxW = bigScores ? 56 : 44;
  const int16_t scoreBoxH = bigScores ? 36 : 28;

  tft.fillRect(leftLogoX, logoY, logoSize, logoSize, Palette::PANEL);
  tft.fillRect(rightLogoX, logoY, logoSize, logoSize, Palette::PANEL);
  if (showScores) {
    tft.fillRect((int16_t)(leftScoreX - scoreBoxW / 2), (int16_t)(scoreY - scoreBoxH / 2), scoreBoxW, scoreBoxH, Palette::PANEL);
    tft.fillRect((int16_t)(rightScoreX - scoreBoxW / 2), (int16_t)(scoreY - scoreBoxH / 2), scoreBoxW, scoreBoxH, Palette::PANEL);
  }

  const bool canShowAbbr = showAbbr && (logoSize <= 72);
  if (canShowAbbr) {
    const int16_t abbrY = (int16_t)(logoY + logoSize + 12);
    tft.fillRect((int16_t)(leftLogoX - 2), (int16_t)(abbrY - 10), (int16_t)(logoSize + 4), 20, Palette::PANEL);
    tft.fillRect((int16_t)(rightLogoX - 2), (int16_t)(abbrY - 10), (int16_t)(logoSize + 4), 20, Palette::PANEL);
  }

  Assets::drawLogo(tft, away.abbr, leftLogoX, logoY, logoSize);
  Assets::drawLogo(tft, home.abbr, rightLogoX, logoY, logoSize);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(Palette::WHITE, Palette::PANEL);

  if (showScores) {
    tft.setTextFont(scoreFont);
    tft.drawString(String(away.score), leftScoreX, scoreY);
    tft.drawString(String(home.score), rightScoreX, scoreY);
  }

  String mid = midLabel;
  if (mid.isEmpty() && showScores) mid = "-";
  if (mid.length()) {
    tft.setTextFont(bigScores ? 4 : 2);
    tft.drawString(mid, dashX, scoreY);
  }

  if (canShowAbbr) {
    const int16_t abbrY = (int16_t)(logoY + logoSize + 12);
    tft.setTextFont(2);
    tft.drawString(away.abbr, (int16_t)(leftLogoX + logoSize / 2), abbrY);
    tft.drawString(home.abbr, (int16_t)(rightLogoX + logoSize / 2), abbrY);
  }
}
