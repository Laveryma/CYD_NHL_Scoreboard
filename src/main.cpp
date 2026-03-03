#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <time.h>

#include "ui.h"
#include "nhl_client.h"
#include "types.h"
#include "assets.h"
#include "wifi_fallback.h"
#include "anthem.h"

// Create `include/config.h` from the example.
#include "config.h"

static TFT_eSPI tft;
static Ui ui;
static NhlClient nhl;

static GameState g;
// Keep large state temporaries off the loop stack to avoid stack pressure/reset risk.
static GameState gScoreboardScratch;
static GameState gDetailScratch;
static ScreenMode mode = ScreenMode::NO_GAME;

static bool manualOverride = false;
static uint8_t manualIndex = 0;
static const ScreenMode kManualScreens[] = {
  ScreenMode::LAST_GAME,
  ScreenMode::STANDINGS,
  ScreenMode::LIVE,
  ScreenMode::INTERMISSION,
  ScreenMode::FINAL,
  ScreenMode::GOAL,
  ScreenMode::NO_GAME
};
static const uint8_t kManualScreenCount = sizeof(kManualScreens) / sizeof(kManualScreens[0]);

static bool bootBtnLastRead = true;
static bool bootBtnStable = true;
static uint32_t bootBtnLastChange = 0;
static uint32_t bootBtnPressedAt = 0;
static bool bootBtnLongPressHandled = false;
static const uint32_t kBootBtnLongPressMs = 1400;
static uint32_t lastScoreboardPoll = 0;
static uint32_t lastDetailPoll = 0;
static uint32_t lastLastGamePoll = 0;
static uint32_t lastStandingsPoll = 0;
static uint32_t lastPregameTickerPoll = 0;

static uint32_t goalBannerUntil = 0;
static uint32_t lastSeenGoalEvent = 0;
static uint32_t lastGoodFetchMs = 0;
static bool lastStaleShown = true;
static bool lastWifiShown = false;

static const uint32_t DATA_STALE_MS = 60000;
static const uint32_t LAST_GAME_POLL_MS = 30UL * 60UL * 1000UL;
static const uint32_t STANDINGS_POLL_MS = 15UL * 60UL * 1000UL;
static const uint32_t LAST_GAME_RETRY_MS = 20UL * 1000UL;
static const uint32_t STANDINGS_RETRY_MS = 20UL * 1000UL;
static const uint32_t PREGAME_TICKER_POLL_MS = 60UL * 1000UL;

static bool timeConfigured = false;
static uint32_t lastTimeConfigAttempt = 0;

static uint32_t lastNextGamePoll = 0;
static uint32_t lastNoGameRedraw = 0;
static bool finalHoldActive = false;
static String finalHoldGameId;
static String finalDismissedGameId;
static bool finalDismissPreviewActive = false;
static uint32_t finalDismissPreviewUntil = 0;
static const uint32_t FINAL_DISMISS_STANDINGS_MS = 16000;

struct GoalEvent {
  uint32_t eventId = 0;
  String goalText;
  String goalTeamAbbr;
  String goalScorer;
  bool leafsJustScored = false;
};

static const uint8_t kGoalQueueSize = 4;
static GoalEvent goalQueue[kGoalQueueSize];
static uint8_t goalHead = 0;
static uint8_t goalTail = 0;
static uint8_t goalCount = 0;

static void ensureTimeConfigured(uint32_t now) {
  if (timeConfigured) return;

  // Throttle so we do not spam configTime.
  if (now - lastTimeConfigAttempt < 15000) return;
  lastTimeConfigAttempt = now;

  // Set local timezone for display formatting (strftime/localtime).
  setenv("TZ", TZ_INFO, 1);
  tzset();

  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  // We consider time "configured" once the epoch looks plausible.
  // (SNTP runs asynchronously in the background.)
  timeConfigured = (time(nullptr) > 1577836800);
}

static const char *modeName(ScreenMode m) {
  switch (m) {
    case ScreenMode::NO_GAME: return "NO_GAME";
    case ScreenMode::LIVE: return "LIVE";
    case ScreenMode::INTERMISSION: return "INTERMISSION";
    case ScreenMode::FINAL: return "FINAL";
    case ScreenMode::LAST_GAME: return "LAST_GAME";
    case ScreenMode::STANDINGS: return "STANDINGS";
    case ScreenMode::GOAL: return "GOAL";
    default: return "UNKNOWN";
  }
}

static void logModeChange(ScreenMode from, ScreenMode to, const char *reason) {
  if (from == to) return;
  if (reason && reason[0]) {
    Serial.printf("STATE: %s -> %s (%s)\n", modeName(from), modeName(to), reason);
  } else {
    Serial.printf("STATE: %s -> %s\n", modeName(from), modeName(to));
  }
}

static void refreshMeta(uint32_t now) {
  g.wifiConnected = (WiFi.status() == WL_CONNECTED);
  g.dataStale = (lastGoodFetchMs == 0) || (now - lastGoodFetchMs > DATA_STALE_MS);
  g.lastGoodFetchMs = lastGoodFetchMs;
}

static ScreenMode computeMode(const GameState &st) {
  if (!st.hasGame || st.isPre) return ScreenMode::NO_GAME;
  if (st.isFinal) {
    if (st.gameId.length() && st.gameId == finalDismissedGameId) return ScreenMode::NO_GAME;
    return ScreenMode::FINAL;
  }
  if (st.isIntermission) return ScreenMode::INTERMISSION;
  if (st.isLive) return ScreenMode::LIVE;
  return ScreenMode::LIVE;
}

static void syncFinalFlow(const GameState &st) {
  // Once the feed moves away from the dismissed final game, clear the dismissal.
  if (finalDismissedGameId.length()) {
    if (!st.isFinal || st.gameId != finalDismissedGameId) {
      finalDismissedGameId = "";
    }
  }

  // If a different game appears, stop holding the previous final.
  if (finalHoldActive && st.gameId.length() && st.gameId != finalHoldGameId) {
    finalHoldActive = false;
    finalHoldGameId = "";
  }
}

static ScreenMode computeAutoMode(const GameState &st) {
  if (finalDismissPreviewActive) {
    return ScreenMode::STANDINGS;
  }

  syncFinalFlow(st);

  if (finalHoldActive) {
    return ScreenMode::FINAL;
  }

  ScreenMode nextMode = computeMode(st);
  if (nextMode == ScreenMode::FINAL && st.gameId.length() && st.gameId != finalDismissedGameId) {
    finalHoldActive = true;
    finalHoldGameId = st.gameId;
    return ScreenMode::FINAL;
  }
  return nextMode;
}

static void render(ScreenMode m, const GameState &st) {
  switch (m) {
    case ScreenMode::NO_GAME:       ui.drawNoGame(st, FOCUS_TEAM_ABBR); break;
    case ScreenMode::LIVE:          ui.drawLive(st); break;
    case ScreenMode::INTERMISSION:  ui.drawIntermission(st); break;
    case ScreenMode::FINAL:         ui.drawFinal(st); break;
    case ScreenMode::LAST_GAME:     ui.drawLastGame(st); break;
    case ScreenMode::STANDINGS:     ui.drawStandings(st, FOCUS_TEAM_ABBR); break;
    case ScreenMode::GOAL:          ui.drawGoal(st); break;
  }
}

static void applyManualScreen() {
  if (manualOverride) {
    ScreenMode target = kManualScreens[manualIndex];
    logModeChange(mode, target, "manual");
    mode = target;
    render(mode, g);
  } else {
    ScreenMode nextMode = computeAutoMode(g);
    logModeChange(mode, nextMode, "auto");
    mode = nextMode;
    render(mode, g);
  }
}

static bool goalQueueContains(uint32_t eventId) {
  for (uint8_t i = 0; i < goalCount; ++i) {
    uint8_t idx = (uint8_t)((goalHead + i) % kGoalQueueSize);
    if (goalQueue[idx].eventId == eventId) return true;
  }
  return false;
}

static void enqueueGoalEvent(const GoalEvent &ev) {
  if (ev.eventId == 0) return;
  if (goalQueueContains(ev.eventId)) return;
  if (goalCount >= kGoalQueueSize) {
    goalHead = (uint8_t)((goalHead + 1) % kGoalQueueSize);
    goalCount--;
  }
  goalQueue[goalTail] = ev;
  goalTail = (uint8_t)((goalTail + 1) % kGoalQueueSize);
  goalCount++;
}

static bool dequeueGoalEvent(GoalEvent &out) {
  if (goalCount == 0) return false;
  out = goalQueue[goalHead];
  goalHead = (uint8_t)((goalHead + 1) % kGoalQueueSize);
  goalCount--;
  return true;
}

static void showGoalEvent(const GoalEvent &ev, uint32_t now) {
  g.goalText = ev.goalText;
  g.goalTeamAbbr = ev.goalTeamAbbr;
  g.goalScorer = ev.goalScorer;
  g.leafsJustScored = ev.leafsJustScored;
  g.lastGoalEventId = ev.eventId;

  logModeChange(mode, ScreenMode::GOAL, "goal");
  mode = ScreenMode::GOAL;
  render(mode, g);
  goalBannerUntil = now + 9000;
}

static void maybeShowQueuedGoal(uint32_t now) {
  if (manualOverride || goalBannerUntil > now || mode == ScreenMode::GOAL) return;
  GoalEvent ev;
  if (dequeueGoalEvent(ev)) {
    showGoalEvent(ev, now);
  }
}

static void handleBootButton(uint32_t now) {
  const bool read = (digitalRead(BOOT_BTN_PIN) == HIGH);
  if (read != bootBtnLastRead) {
    bootBtnLastRead = read;
    bootBtnLastChange = now;
  }
  if ((now - bootBtnLastChange) < 40) return;

  if (read != bootBtnStable) {
    bootBtnStable = read;
    if (!bootBtnStable) {
      bootBtnPressedAt = now;
      bootBtnLongPressHandled = false;
      if (!manualOverride && mode == ScreenMode::FINAL && finalHoldActive) {
        finalDismissedGameId = finalHoldGameId.length() ? finalHoldGameId : g.gameId;
        finalHoldActive = false;
        finalHoldGameId = "";
        finalDismissPreviewActive = true;
        finalDismissPreviewUntil = now + FINAL_DISMISS_STANDINGS_MS;
        if (WiFi.status() == WL_CONNECTED && !g.standings.hasData) {
          nhl.fetchConferenceStandings(g, FOCUS_TEAM_ABBR);
          lastStandingsPoll = now;
        }
        logModeChange(mode, ScreenMode::STANDINGS, "final-dismiss");
        mode = ScreenMode::STANDINGS;
        render(mode, g);
        return;
      }
      if (!manualOverride) {
        finalDismissPreviewActive = false;
        finalDismissPreviewUntil = 0;
        manualOverride = true;
        manualIndex = 0;
      } else {
        manualIndex++;
        if (manualIndex >= kManualScreenCount) {
          manualOverride = false;
          manualIndex = 0;
        }
      }
      applyManualScreen();
    } else {
      bootBtnPressedAt = 0;
      bootBtnLongPressHandled = false;
    }
  }

  if (!bootBtnStable &&
      !bootBtnLongPressHandled &&
      bootBtnPressedAt > 0 &&
      (now - bootBtnPressedAt >= kBootBtnLongPressMs)) {
    bootBtnLongPressHandled = true;
    Serial.println("BOOT: long press -> anthem test");
    Anthem::playNow();
  }
}

void setup() {
  Serial.begin(115200);

  // Backlight PWM
  ledcSetup(CYD_BL_PWM_CH, 5000, 8);
  ledcAttachPin(TFT_BL, CYD_BL_PWM_CH);

  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  bootBtnLastRead = (digitalRead(BOOT_BTN_PIN) == HIGH);
  bootBtnStable = bootBtnLastRead;
  bootBtnLastChange = millis();

  uint8_t rotation = TFT_ROTATION;
  ui.begin(tft, rotation);
  if (tft.width() < tft.height()) {
    rotation = (rotation == 1) ? 3 : 1;
    ui.setRotation(rotation);
  }
  ui.setBacklight(85);

  Assets::begin(tft);
  Anthem::begin();
  ui.drawBootSplash("LANDSCAPE MODE", "CONNECTING TO WIFI");

  wifiConnectWithFallback();

  const uint32_t now = millis();
  ensureTimeConfigured(now);

  // Force an immediate first poll once Wi-Fi is up (avoids waiting 15s+ after boot).
  lastScoreboardPoll = now - POLL_SCOREBOARD_MS;
  lastDetailPoll     = now - POLL_GAMEDETAIL_MS;
  lastNextGamePoll   = now - (31UL * 60UL * 1000UL);
  lastLastGamePoll   = now - LAST_GAME_POLL_MS;
  lastStandingsPoll  = now - STANDINGS_POLL_MS;
  lastPregameTickerPoll = now - PREGAME_TICKER_POLL_MS;

  if (WiFi.status() == WL_CONNECTED) {
    if (nhl.fetchScoreboardNow(gScoreboardScratch, FOCUS_TEAM_ABBR)) {
      gScoreboardScratch.last = g.last;
      gScoreboardScratch.standings = g.standings;
      gScoreboardScratch.hasNextGame = g.hasNextGame;
      gScoreboardScratch.nextOppAbbr = g.nextOppAbbr;
      gScoreboardScratch.nextIsHome = g.nextIsHome;
      gScoreboardScratch.nextIsPlayoff = g.nextIsPlayoff;
      gScoreboardScratch.nextVenue = g.nextVenue;
      gScoreboardScratch.nextCity = g.nextCity;
      gScoreboardScratch.nextStartEpoch = g.nextStartEpoch;
      gScoreboardScratch.pregameTicker = gScoreboardScratch.isPre ? g.pregameTicker : String("");
      g = gScoreboardScratch;
      lastGoodFetchMs = now;
      refreshMeta(now);
      lastScoreboardPoll = now;

      if (!g.hasGame || g.isPre) {
        nhl.fetchNextGame(g, FOCUS_TEAM_ABBR);
        lastNextGamePoll = now;
      }
      if (g.hasGame && g.isPre) {
        nhl.fetchPregameTicker(g, FOCUS_TEAM_ABBR);
        lastPregameTickerPoll = now;
      } else {
        g.pregameTicker = "";
      }

      // Defer heavier auxiliary fetches to loop() so first render is fast.
      lastLastGamePoll = now - LAST_GAME_RETRY_MS;
      lastStandingsPoll = now - STANDINGS_RETRY_MS;

      ScreenMode nextMode = computeAutoMode(g);
      logModeChange(mode, nextMode, "boot");
      mode = nextMode;
      render(mode, g);
    } else {
      refreshMeta(now);
      nhl.fetchLastGameRecap(g, FOCUS_TEAM_ABBR);
      lastLastGamePoll = now;
      nhl.fetchConferenceStandings(g, FOCUS_TEAM_ABBR);
      lastStandingsPoll = now;
      render(ScreenMode::NO_GAME, g);
    }
  } else {
    refreshMeta(now);
    render(ScreenMode::NO_GAME, g);
  }

  Anthem::prime(g);
}

void loop() {
  wifiTick();
  const uint32_t now = millis();

  handleBootButton(now);

  if (WiFi.status() == WL_CONNECTED) {
    ensureTimeConfigured(now);
  }

  refreshMeta(now);
  if (finalDismissPreviewActive && !manualOverride && mode == ScreenMode::STANDINGS && now >= finalDismissPreviewUntil) {
    finalDismissPreviewActive = false;
    finalDismissPreviewUntil = 0;
    if (WiFi.status() == WL_CONNECTED) {
      nhl.fetchNextGame(g, FOCUS_TEAM_ABBR);
      lastNextGamePoll = now;
    }
    logModeChange(mode, ScreenMode::NO_GAME, "final-dismiss-timeout");
    mode = ScreenMode::NO_GAME;
    render(mode, g);
  }

  if (g.dataStale != lastStaleShown || g.wifiConnected != lastWifiShown) {
    lastStaleShown = g.dataStale;
    lastWifiShown = g.wifiConnected;
    if (!(mode == ScreenMode::GOAL && goalBannerUntil > now)) {
      applyManualScreen();
    }
  }

  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  // Poll scoreboard
  if (wifiConnected && now - lastScoreboardPoll >= POLL_SCOREBOARD_MS) {
    lastScoreboardPoll = now;

    if (nhl.fetchScoreboardNow(gScoreboardScratch, FOCUS_TEAM_ABBR)) {
      const String prevGameId = g.gameId;
      const String prevHomeAbbr = g.home.abbr;
      const String prevAwayAbbr = g.away.abbr;
      const TeamLine prevHome = g.home;
      const TeamLine prevAway = g.away;
      gScoreboardScratch.last = g.last;
      gScoreboardScratch.standings = g.standings;
      gScoreboardScratch.hasNextGame = g.hasNextGame;
      gScoreboardScratch.nextOppAbbr = g.nextOppAbbr;
      gScoreboardScratch.nextIsHome = g.nextIsHome;
      gScoreboardScratch.nextIsPlayoff = g.nextIsPlayoff;
      gScoreboardScratch.nextVenue = g.nextVenue;
      gScoreboardScratch.nextCity = g.nextCity;
      gScoreboardScratch.nextStartEpoch = g.nextStartEpoch;
      gScoreboardScratch.pregameTicker = gScoreboardScratch.isPre ? g.pregameTicker : String("");
      if (!prevGameId.isEmpty()
          && gScoreboardScratch.gameId == prevGameId
          && gScoreboardScratch.home.abbr == prevHomeAbbr
          && gScoreboardScratch.away.abbr == prevAwayAbbr) {
        gScoreboardScratch.home.sog = prevHome.sog;
        gScoreboardScratch.home.hits = prevHome.hits;
        gScoreboardScratch.home.foPct = prevHome.foPct;
        gScoreboardScratch.away.sog = prevAway.sog;
        gScoreboardScratch.away.hits = prevAway.hits;
        gScoreboardScratch.away.foPct = prevAway.foPct;
      }
      g = gScoreboardScratch;
      lastGoodFetchMs = now;
      refreshMeta(now);
      Anthem::tick(g);

      // If there is no game today, fetch the next upcoming game (schedule).
      if ((!g.hasGame || g.isPre) && (!g.hasNextGame || (now - lastNextGamePoll >= 30UL * 60UL * 1000UL))) {
        lastNextGamePoll = now;
        nhl.fetchNextGame(g, FOCUS_TEAM_ABBR);
      }
      if (g.hasGame && g.isPre && (g.pregameTicker.length() == 0 || now - lastPregameTickerPoll >= PREGAME_TICKER_POLL_MS)) {
        lastPregameTickerPoll = now;
        nhl.fetchPregameTicker(g, FOCUS_TEAM_ABBR);
      } else if (!g.isPre) {
        g.pregameTicker = "";
      }

      if (goalBannerUntil <= now && !manualOverride) {
        ScreenMode nextMode = computeAutoMode(g);
        logModeChange(mode, nextMode, "scoreboard");
        mode = nextMode;
        render(mode, g);
      }
    } else {
      Serial.println("Scoreboard fetch failed");
    }
  }

  const uint32_t lastGamePollInterval = g.last.hasGame ? LAST_GAME_POLL_MS : LAST_GAME_RETRY_MS;
  if (wifiConnected && now - lastLastGamePoll >= lastGamePollInterval) {
    lastLastGamePoll = now;
    const bool ok = nhl.fetchLastGameRecap(g, FOCUS_TEAM_ABBR);
    if (!ok) {
      Serial.println("Last game recap fetch failed");
    } else if (mode == ScreenMode::LAST_GAME) {
      render(mode, g);
    }
  }

  const uint32_t standingsPollInterval = g.standings.hasData ? STANDINGS_POLL_MS : STANDINGS_RETRY_MS;
  if (wifiConnected && now - lastStandingsPoll >= standingsPollInterval) {
    lastStandingsPoll = now;
    if (nhl.fetchConferenceStandings(g, FOCUS_TEAM_ABBR)) {
      if (mode == ScreenMode::STANDINGS) {
        render(mode, g);
      }
    } else {
      Serial.println("Conference standings fetch failed");
    }
  }

  // If we're on the NO_GAME screen and we have a next game, redraw periodically
  // so the countdown updates (time is updated asynchronously via SNTP).
  if (mode == ScreenMode::NO_GAME && (g.hasNextGame || g.isPre)) {
    const uint32_t noGameRedrawMs = g.isPre ? 120UL : 1000UL;
    if (now - lastNoGameRedraw >= noGameRedrawMs) {
      lastNoGameRedraw = now;
      ui.drawNoGame(g, FOCUS_TEAM_ABBR);
    }
  }

  // During active game (incl. intermission/crit), poll detail + goal detection
  if (wifiConnected && g.hasGame && !g.isFinal && !g.isPre && (now - lastDetailPoll >= POLL_GAMEDETAIL_MS)) {
    lastDetailPoll = now;

    nhl.fetchGameBoxscore(g);

    gDetailScratch = g;
    const bool gotGoal = nhl.fetchLatestGoal(gDetailScratch, FOCUS_TEAM_ABBR);
    g.strengthLabel = gDetailScratch.strengthLabel;
    g.strengthColour = gDetailScratch.strengthColour;
    g.hasPenaltyCountdown = gDetailScratch.hasPenaltyCountdown;
    g.penaltyCountdown = gDetailScratch.penaltyCountdown;
    if (gDetailScratch.home.foPct >= 0 && gDetailScratch.away.foPct >= 0) {
      g.home.foPct = gDetailScratch.home.foPct;
      g.away.foPct = gDetailScratch.away.foPct;
    }
    if (gotGoal) {
      if (gDetailScratch.lastGoalEventId != 0 && gDetailScratch.lastGoalEventId != lastSeenGoalEvent) {
        lastSeenGoalEvent = gDetailScratch.lastGoalEventId;
        GoalEvent ev;
        ev.eventId = gDetailScratch.lastGoalEventId;
        ev.goalText = gDetailScratch.goalText;
        ev.goalTeamAbbr = gDetailScratch.goalTeamAbbr;
        ev.goalScorer = gDetailScratch.goalScorer;
        ev.leafsJustScored = gDetailScratch.leafsJustScored;
        enqueueGoalEvent(ev);
      }
    }
  }

  if (!manualOverride) {
    maybeShowQueuedGoal(now);
  }

  // Goal banner timeout
  if (!manualOverride && goalBannerUntil > 0 && goalBannerUntil <= now && mode == ScreenMode::GOAL) {
    goalBannerUntil = 0;
    GoalEvent ev;
    if (dequeueGoalEvent(ev)) {
      showGoalEvent(ev, now);
    } else {
      ScreenMode nextMode = computeAutoMode(g);
      logModeChange(mode, nextMode, "goal-timeout");
      mode = nextMode;
      render(mode, g);
    }
  }

  delay(10);
}
