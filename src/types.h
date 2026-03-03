#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <time.h>

enum class ScreenMode : uint8_t {
  LIVE,
  GOAL,
  INTERMISSION,
  FINAL,
  LAST_GAME,
  STANDINGS,
  NO_GAME
};

struct TeamLine {
  String abbr;
  int score = 0;
  int sog = -1;
  int hits = -1;
  int foPct = -1;
};

static const uint8_t kRecapMaxScorers = 3;
static const uint8_t kRecapMaxPeriods = 5;
static const uint8_t kConferenceStandingsMaxTeams = 16;

struct ScorerEntry {
  String name;
  uint8_t goals = 0;
};

struct PeriodEntry {
  String label;
  uint8_t home = 0;
  uint8_t away = 0;
};

struct LastGameRecap {
  bool hasGame = false;
  String gameId;
  TeamLine away;
  TeamLine home;
  time_t startEpoch = 0;
  String venue;
  String city;
  uint8_t awayScorerCount = 0;
  ScorerEntry awayScorers[kRecapMaxScorers];
  uint8_t homeScorerCount = 0;
  ScorerEntry homeScorers[kRecapMaxScorers];
  uint8_t periodCount = 0;
  PeriodEntry periods[kRecapMaxPeriods];
};

struct ConferenceStandingEntry {
  uint8_t rank = 0;
  uint8_t divisionRank = 0;
  String abbr;
  String divisionAbbrev;
  String divisionName;
  uint8_t gamesPlayed = 0;
  uint16_t points = 0;
  uint8_t wins = 0;
  uint8_t losses = 0;
  uint8_t otLosses = 0;
  uint8_t l10Wins = 0;
  uint8_t l10Losses = 0;
  uint8_t l10OtLosses = 0;
  char streakCode = '-';
  uint8_t streakCount = 0;
};

struct ConferenceStandings {
  bool hasData = false;
  String conferenceAbbrev;
  String conferenceName;
  uint8_t teamCount = 0;
  uint8_t focusIndex = 255;
  ConferenceStandingEntry teams[kConferenceStandingsMaxTeams];
};

struct GameState {
  bool hasGame = false;
  bool isFinal = false;
  bool isIntermission = false;
  bool isLive = false;
  bool isPre = false;
  bool isPlayoff = false;

  String gameId;
  String startTimeHHMM;
  time_t startEpoch = 0;
  String clock;
  int period = 0;
  String periodType;

  String strengthLabel;
  uint16_t strengthColour = 0;
  bool hasPenaltyCountdown = false;
  String penaltyCountdown;

  TeamLine away;
  TeamLine home;

  uint32_t lastGoalEventId = 0;
  bool leafsJustScored = false;
  String goalTeamAbbr;
  String goalScorer;
  String goalText;

  // Next game fallback (when there is no game today).
  bool hasNextGame = false;
  String nextOppAbbr;
  bool nextIsHome = false;
  bool nextIsPlayoff = false;
  String nextVenue;
  String nextCity;
  time_t nextStartEpoch = 0;
  String pregameTicker;

  // Data freshness / connectivity (set by main loop).
  bool dataStale = false;
  bool wifiConnected = false;
  uint32_t lastGoodFetchMs = 0;

  // Last game recap.
  LastGameRecap last;

  // Favourite-team conference standings.
  ConferenceStandings standings;
};
