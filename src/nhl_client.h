#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "types.h"

class NhlClient {
public:
  bool fetchScoreboardNow(GameState &out, const String &focusTeamAbbr);
  bool fetchGameBoxscore(GameState &io);
  bool fetchLatestGoal(GameState &io, const String &focusTeamAbbr);
  bool fetchNextGame(GameState &io, const String &focusTeamAbbr);
  bool fetchLastGameRecap(GameState &io, const String &focusTeamAbbr);
  bool fetchConferenceStandings(GameState &io, const String &focusTeamAbbr);
  bool fetchPregameTicker(GameState &io, const String &focusTeamAbbr);

private:
  bool httpGetJson(const String &url, JsonDocument &doc);
  bool httpGetJson(const String &url, JsonDocument &doc, const JsonDocument &filter);
  static String hhmmFromIsoUtc(const String &iso);
};
