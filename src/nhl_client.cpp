#include "nhl_client.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <limits.h>
#include <math.h>
#include <time.h>

// NHL "api-web" base URL (unofficial but widely used).
static const char *kBase = "https://api-web.nhle.com/v1";

static const char *wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

static void logWifiState() {
  wl_status_t status = WiFi.status();
  String ssid = WiFi.SSID();
  int32_t rssi = (status == WL_CONNECTED) ? WiFi.RSSI() : 0;
  IPAddress ip = WiFi.localIP();
  Serial.printf("WiFi: %s SSID=%s RSSI=%ld IP=%s\n",
                wifiStatusToString(status),
                ssid.c_str(),
                (long)rssi,
                ip.toString().c_str());
}

// ESP32 Arduino toolchains differ: some expose `timegm()`, some don't.
// We only need a UTC ISO-8601 timestamp -> Unix epoch seconds converter,
// so we implement a small, portable one.
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  // Days since 1970-01-01 (UTC), Gregorian calendar.
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool utcIsoToEpoch(const String &iso, time_t &outEpoch) {
  int y=0, mo=0, d=0, hh=0, mm=0, ss=0;
  int n = sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss);
  if (n < 5) return false;
  if (n == 5) ss = 0;

  const int64_t days = daysFromCivil(y, (unsigned)mo, (unsigned)d);
  const int64_t secs = days * 86400LL + (int64_t)hh * 3600LL + (int64_t)mm * 60LL + (int64_t)ss;
  outEpoch = (time_t)secs;
  return true;
}

static bool parseIsoUtcToEpoch(const String &iso, time_t &outEpoch) {
  return utcIsoToEpoch(iso, outEpoch);
}

static String jsonStringOrDefault(const JsonVariantConst &v) {
  if (v.is<const char *>()) return String(v.as<const char *>());
  if (v.is<JsonObjectConst>()) {
    const char *d = v["default"] | "";
    return String(d ? d : "");
  }
  return String("");
}

static String playerDisplayName(const JsonObjectConst &p) {
  String full = jsonStringOrDefault(p["name"]);
  if (full.length()) return full;

  const String first = jsonStringOrDefault(p["firstName"]);
  const String last = jsonStringOrDefault(p["lastName"]);
  if (first.length() && last.length()) return first + " " + last;
  if (last.length()) return last;
  if (first.length()) return first;
  return String("");
}

static void appendTickerNames(const JsonArrayConst &arr,
                              String &out,
                              uint8_t &count,
                              uint8_t maxCount) {
  if (arr.isNull()) return;
  for (JsonObjectConst p : arr) {
    if (count >= maxCount) break;
    const String name = playerDisplayName(p);
    if (!name.length()) continue;
    if (out.length()) out += "  |  ";
    out += name;
    count++;
  }
}

static void clearRecap(LastGameRecap &recap) {
  recap = LastGameRecap();
}

static void addScorer(ScorerEntry *list, uint8_t &count, const String &name) {
  if (name.isEmpty()) return;
  for (uint8_t i = 0; i < count; ++i) {
    if (list[i].name == name) {
      if (list[i].goals < 255) list[i].goals++;
      return;
    }
  }
  if (count < kRecapMaxScorers) {
    list[count].name = name;
    list[count].goals = 1;
    count++;
  }
}

static String periodLabelFromDescriptor(const JsonObjectConst &pd) {
  const char *type_c = pd["periodType"] | "";
  const String type = type_c ? type_c : "";
  const int num = pd["number"] | 0;
  if (type == "REG") {
    return String("P") + String(num);
  }
  if (type == "OT") return String("OT");
  if (type == "SO") return String("SO");
  if (num > 0) return String("P") + String(num);
  return String("P");
}

static int sumHitsFromArray(const JsonArrayConst &arr) {
  int total = 0;
  for (JsonObjectConst p : arr) {
    total += p["hits"] | 0;
  }
  return total;
}

static int sumHitsFromTeam(const JsonObjectConst &team) {
  int total = 0;
  JsonArrayConst forwards = team["forwards"].as<JsonArrayConst>();
  if (!forwards.isNull()) total += sumHitsFromArray(forwards);
  JsonArrayConst defense = team["defense"].as<JsonArrayConst>();
  if (!defense.isNull()) total += sumHitsFromArray(defense);
  JsonArrayConst goalies = team["goalies"].as<JsonArrayConst>();
  if (!goalies.isNull()) total += sumHitsFromArray(goalies);
  return total;
}

static void applyStrengthFromSituation(GameState &io,
                                       const String &code,
                                       const String &homeAbbr,
                                       const String &awayAbbr) {
  if (code.length() != 4) {
    io.strengthLabel = "EVEN STRENGTH";
    return;
  }
  const int awayGoalie = code.charAt(0) - '0';
  const int awaySkaters = code.charAt(1) - '0';
  const int homeSkaters = code.charAt(2) - '0';
  const int homeGoalie = code.charAt(3) - '0';

  const bool goaliesPresent = (awayGoalie == 1 && homeGoalie == 1);
  if (goaliesPresent && awaySkaters != homeSkaters) {
    if (awaySkaters > homeSkaters) {
      io.strengthLabel = awayAbbr + " POWER PLAY";
    } else {
      io.strengthLabel = homeAbbr + " POWER PLAY";
    }
    return;
  }
  io.strengthLabel = "EVEN STRENGTH";
}

static bool parseSituationCode(const String &code,
                               int &awaySkaters,
                               int &homeSkaters,
                               bool &goaliesPresent) {
  if (code.length() != 4) return false;
  const int awayGoalie = code.charAt(0) - '0';
  awaySkaters = code.charAt(1) - '0';
  homeSkaters = code.charAt(2) - '0';
  const int homeGoalie = code.charAt(3) - '0';
  goaliesPresent = (awayGoalie == 1 && homeGoalie == 1);
  return (awaySkaters >= 0 && awaySkaters <= 6 && homeSkaters >= 0 && homeSkaters <= 6);
}

static int parseClockToSeconds(const String &mmss) {
  int mm = 0, ss = 0;
  if (sscanf(mmss.c_str(), "%d:%d", &mm, &ss) != 2) return -1;
  if (mm < 0 || ss < 0 || ss >= 60) return -1;
  return mm * 60 + ss;
}

static int inferredPeriodLengthSeconds(uint8_t periodNum, const String &periodType, bool isPlayoff) {
  if (periodType == "REG") return 20 * 60;
  if (periodType == "OT") return isPlayoff ? (20 * 60) : (5 * 60);
  if (periodType == "SO") return 0;

  // Fallback inference when periodType is missing.
  if (periodNum <= 3) return 20 * 60;
  if (!isPlayoff && periodNum == 4) return 5 * 60;
  if (!isPlayoff && periodNum >= 5) return 0;
  return 20 * 60;
}

static int elapsedBeforePeriod(uint8_t periodNum, bool isPlayoff) {
  if (periodNum <= 1) return 0;
  int total = 0;
  for (uint8_t p = 1; p < periodNum; ++p) {
    if (p <= 3) total += 20 * 60;
    else if (!isPlayoff && p == 4) total += 5 * 60;
    else if (!isPlayoff && p >= 5) total += 0;
    else total += 20 * 60;
  }
  return total;
}

static int elapsedFromTimeInPeriod(uint8_t periodNum,
                                   const String &periodType,
                                   const String &timeInPeriod,
                                   bool isPlayoff) {
  const int elapsedInPeriod = parseClockToSeconds(timeInPeriod);
  if (elapsedInPeriod < 0) return -1;
  return elapsedBeforePeriod(periodNum, isPlayoff) + elapsedInPeriod;
}

static int elapsedFromCurrentClock(uint8_t periodNum,
                                   const String &periodType,
                                   int secondsRemaining,
                                   bool isPlayoff) {
  const int periodLen = inferredPeriodLengthSeconds(periodNum, periodType, isPlayoff);
  if (periodLen < 0) return -1;
  if (secondsRemaining < 0) return -1;
  if (secondsRemaining > periodLen) secondsRemaining = periodLen;
  const int elapsedInPeriod = periodLen - secondsRemaining;
  return elapsedBeforePeriod(periodNum, isPlayoff) + elapsedInPeriod;
}

static bool teamIdToHomeFlag(int teamId, int homeId, int awayId, bool &onHome) {
  if (teamId == homeId) {
    onHome = true;
    return true;
  }
  if (teamId == awayId) {
    onHome = false;
    return true;
  }
  return false;
}

struct ActivePenalty {
  bool active = false;
  bool onHome = false;
  int startElapsed = 0;
  int durationSec = 0;
  bool releasableOnPpGoal = true;
};

static String formatPenaltyCountdown(int remainingSec) {
  if (remainingSec < 0) remainingSec = 0;
  const int mins = remainingSec / 60;
  const int secs = remainingSec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "PP %02d:%02d", mins, secs);
  return String(buf);
}

class ChunkedStream : public Stream {
public:
  explicit ChunkedStream(Stream &src) : _src(src) {}

  int available() override {
    if (_done) return 0;
    if (_peeked >= 0) return 1;
    if (_remaining > 0) {
      int avail = _src.available();
      if (avail > _remaining) avail = (int)_remaining;
      return avail;
    }
    return _src.available();
  }

  int read() override {
    if (_peeked >= 0) {
      int c = _peeked;
      _peeked = -1;
      return c;
    }
    if (_done) return -1;
    if (_remaining == 0) {
      if (!readChunkHeader()) return -1;
    }
    int c = _src.read();
    if (c < 0) return -1;
    _remaining--;
    if (_remaining == 0) {
      consumeCRLF();
    }
    return c;
  }

  int peek() override {
    if (_peeked < 0) {
      _peeked = read();
    }
    return _peeked;
  }

  void flush() override {}

  size_t write(uint8_t) override { return 0; }

private:
  Stream &_src;
  int _peeked = -1;
  int32_t _remaining = 0;
  bool _done = false;

  bool readChunkHeader() {
    char line[24];
    size_t n = _src.readBytesUntil('\n', line, sizeof(line) - 1);
    if (n == 0) return false;
    line[n] = '\0';
    if (n && line[n - 1] == '\r') line[n - 1] = '\0';
    char *semi = strchr(line, ';');
    if (semi) *semi = '\0';
    _remaining = (int32_t)strtol(line, nullptr, 16);
    if (_remaining == 0) {
      // Consume trailing headers until a blank line.
      while (true) {
        n = _src.readBytesUntil('\n', line, sizeof(line) - 1);
        if (n == 0) break;
        line[n] = '\0';
        if (n == 1 && line[0] == '\r') break;
        if (n == 0) break;
        if (line[0] == '\r') break;
      }
      _done = true;
      return false;
    }
    return true;
  }

  void consumeCRLF() {
    int c1 = _src.read();
    int c2 = _src.read();
    (void)c1;
    (void)c2;
  }
};

static bool httpGetJsonInternal(const String &url, JsonDocument &doc, const JsonDocument *filter) {
  // Ensure HTTPS works reliably without bundling CA roots.
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(12000);

  HTTPClient http;
  const uint32_t timeoutMs = 12000;
  http.setTimeout(timeoutMs);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  const char *headers[] = { "Location", "Content-Type", "Content-Length", "Transfer-Encoding" };
  http.collectHeaders(headers, 4);

  Serial.printf("HTTP GET: %s\n", url.c_str());
  logWifiState();

  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "nhlscoreboard-esp32");
  http.addHeader("Accept", "application/json");
  const uint32_t started = millis();
  int code = http.GET();
  const uint32_t elapsed = millis() - started;

  if (code <= 0) {
    Serial.printf("HTTP error: %s (%d) after %lums\n", http.errorToString(code).c_str(), code, (unsigned long)elapsed);
    if (elapsed >= timeoutMs) {
      Serial.printf("HTTP timeout after %lums\n", (unsigned long)elapsed);
    }
    http.end();
    return false;
  }

  Serial.printf("HTTP status: %d in %lums\n", code, (unsigned long)elapsed);

  if (code != 200) {
    String location = http.header("Location");
    String contentType = http.header("Content-Type");
    String contentLength = http.header("Content-Length");
    if (location.length()) Serial.printf("Location: %s\n", location.c_str());
    if (contentType.length()) Serial.printf("Content-Type: %s\n", contentType.c_str());
    if (contentLength.length()) Serial.printf("Content-Length: %s\n", contentLength.c_str());
    String payload = http.getString();
    if (payload.length()) {
      String preview = payload.substring(0, 200);
      Serial.printf("Body (first 200): %s\n", preview.c_str());
    }
    http.end();
    return false;
  }

  const String transferEncoding = http.header("Transfer-Encoding");
  Stream &stream = http.getStream();
  DeserializationError err;
  if (transferEncoding.equalsIgnoreCase("chunked")) {
    ChunkedStream chunked(stream);
    err = filter
      ? deserializeJson(doc, chunked, DeserializationOption::Filter(*filter))
      : deserializeJson(doc, chunked);
  } else {
    err = filter
      ? deserializeJson(doc, stream, DeserializationOption::Filter(*filter))
      : deserializeJson(doc, stream);
  }
  http.end();
  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
  }
  return !err;
}

bool NhlClient::httpGetJson(const String &url, JsonDocument &doc) {
  return httpGetJsonInternal(url, doc, nullptr);
}

bool NhlClient::httpGetJson(const String &url, JsonDocument &doc, const JsonDocument &filter) {
  return httpGetJsonInternal(url, doc, &filter);
}

String NhlClient::hhmmFromIsoUtc(const String &iso) {
  if (iso.length() >= 16 && iso.charAt(10) == 'T') return iso.substring(11, 16);
  return String("");
}

bool NhlClient::fetchScoreboardNow(GameState &out, const String &focusTeamAbbr) {
  out = GameState{};
  out.hasGame = false;

  JsonDocument filter;
  filter["focusedDate"] = true;
  filter["games"][0]["id"] = true;
  filter["games"][0]["gameType"] = true;
  filter["games"][0]["gameState"] = true;
  filter["games"][0]["startTimeUTC"] = true;
  filter["games"][0]["awayTeam"]["abbrev"] = true;
  filter["games"][0]["awayTeam"]["score"] = true;
  filter["games"][0]["homeTeam"]["abbrev"] = true;
  filter["games"][0]["homeTeam"]["score"] = true;
  filter["games"][0]["clock"]["timeRemaining"] = true;
  filter["games"][0]["clock"]["secondsRemaining"] = true;
  filter["games"][0]["clock"]["running"] = true;
  filter["games"][0]["clock"]["inIntermission"] = true;
  filter["games"][0]["periodDescriptor"]["number"] = true;
  filter["games"][0]["periodDescriptor"]["periodType"] = true;
  filter["gamesByDate"][0]["date"] = true;
  filter["gamesByDate"][0]["games"][0]["id"] = true;
  filter["gamesByDate"][0]["games"][0]["gameType"] = true;
  filter["gamesByDate"][0]["games"][0]["gameState"] = true;
  filter["gamesByDate"][0]["games"][0]["startTimeUTC"] = true;
  filter["gamesByDate"][0]["games"][0]["awayTeam"]["abbrev"] = true;
  filter["gamesByDate"][0]["games"][0]["awayTeam"]["score"] = true;
  filter["gamesByDate"][0]["games"][0]["homeTeam"]["abbrev"] = true;
  filter["gamesByDate"][0]["games"][0]["homeTeam"]["score"] = true;
  filter["gamesByDate"][0]["games"][0]["clock"]["timeRemaining"] = true;
  filter["gamesByDate"][0]["games"][0]["clock"]["secondsRemaining"] = true;
  filter["gamesByDate"][0]["games"][0]["clock"]["running"] = true;
  filter["gamesByDate"][0]["games"][0]["clock"]["inIntermission"] = true;
  filter["gamesByDate"][0]["games"][0]["periodDescriptor"]["number"] = true;
  filter["gamesByDate"][0]["games"][0]["periodDescriptor"]["periodType"] = true;

  auto applyFromGames = [&](JsonArray games, bool &matched) -> bool {
    if (games.isNull()) return false;
    matched = false;

    for (JsonObject g : games) {
      const char *awayAbbr_c = g["awayTeam"]["abbrev"] | "";
      const char *homeAbbr_c = g["homeTeam"]["abbrev"] | "";
      String awayAbbr = awayAbbr_c ? awayAbbr_c : "";
      String homeAbbr = homeAbbr_c ? homeAbbr_c : "";

      if (awayAbbr != focusTeamAbbr && homeAbbr != focusTeamAbbr) continue;

      out.hasGame = true;
      out.gameId = String((int)(g["id"] | 0));
      out.away.abbr = awayAbbr;
      out.home.abbr = homeAbbr;

      out.away.score = g["awayTeam"]["score"] | 0;
      out.home.score = g["homeTeam"]["score"] | 0;
      const int gameType = g["gameType"] | 0;
      out.isPlayoff = (gameType == 3);

      const char *state_c = g["gameState"] | "";
      String state = state_c ? state_c : "";
      out.isLive = (state == "LIVE" || state == "CRIT");
      out.isFinal = (state == "FINAL" || state == "OFF");
      out.isPre = (state == "FUT" || state == "PRE");

      const char *clock_c = g["clock"]["timeRemaining"] | "";
      out.clock = clock_c ? clock_c : "";
      out.period = g["periodDescriptor"]["number"] | 0;
      const char *periodType_c = g["periodDescriptor"]["periodType"] | "";
      out.periodType = periodType_c ? periodType_c : "";

      const int secondsRemaining = g["clock"]["secondsRemaining"] | -1;
      const bool running = g["clock"]["running"] | true;
      const bool inIntermission = g["clock"]["inIntermission"] | false;
      const bool atPeriodEnd = (secondsRemaining == 0) || (out.clock == "00:00");
      out.isIntermission = false;
      if (!out.isFinal && !out.isPre) {
        if (inIntermission || (!running && atPeriodEnd && out.period > 0)) {
          out.isIntermission = true;
        }
      }

      const char *startTime_c = g["startTimeUTC"] | "";
      const String startIso = startTime_c ? startTime_c : "";
      out.startTimeHHMM = hhmmFromIsoUtc(startIso);
      out.startEpoch = 0;
      parseIsoUtcToEpoch(startIso, out.startEpoch);

      out.strengthLabel = "EVEN STRENGTH";
      out.strengthColour = 0x07E0;

      matched = true;
      return true;
    }

    return true;
  };

  JsonDocument doc;
  const String scoreboardUrl = String(kBase) + "/scoreboard/now";
  if (httpGetJson(scoreboardUrl, doc, filter)) {
    JsonArray games = doc["games"].as<JsonArray>();
    bool matched = false;
    if (applyFromGames(games, matched)) {
      out.hasGame = matched;
      return true;
    }

    const String focusedDate = doc["focusedDate"] | "";
    JsonArray dates = doc["gamesByDate"].as<JsonArray>();
    if (!dates.isNull()) {
      for (JsonObject d : dates) {
        const String date = d["date"] | "";
        if (focusedDate.length() && date != focusedDate) continue;
        JsonArray dayGames = d["games"].as<JsonArray>();
        if (applyFromGames(dayGames, matched)) {
          out.hasGame = matched;
          return true;
        }
        break;
      }
    }
  }

  Serial.println("Scoreboard endpoint failed, falling back to schedule");
  doc.clear();
  const String scheduleUrl = String(kBase) + "/club-schedule/" + focusTeamAbbr + "/week/now";
  if (httpGetJson(scheduleUrl, doc, filter)) {
    JsonArray games = doc["games"].as<JsonArray>();
    bool matched = false;
    if (applyFromGames(games, matched)) {
      out.hasGame = matched;
      return true;
    }
  }

  return false;
}

bool NhlClient::fetchGameBoxscore(GameState &io) {
  if (!io.hasGame || io.gameId.isEmpty()) return false;

  JsonDocument doc;
  if (!httpGetJson(String(kBase) + "/gamecenter/" + io.gameId + "/boxscore", doc)) return false;

  io.away.sog = doc["awayTeam"]["sog"] | io.away.sog;
  io.home.sog = doc["homeTeam"]["sog"] | io.home.sog;

  JsonObject teamStats = doc["teamStats"].as<JsonObject>();
  if (!teamStats.isNull()) {
    JsonObject away = teamStats["awayTeam"].as<JsonObject>();
    JsonObject home = teamStats["homeTeam"].as<JsonObject>();

    io.away.hits = away["hits"] | io.away.hits;
    io.home.hits = home["hits"] | io.home.hits;

    float af = away["faceoffWinningPctg"] | -1.0f;
    float hf = home["faceoffWinningPctg"] | -1.0f;
    if (af >= 0) io.away.foPct = (int)lroundf(af);
    if (hf >= 0) io.home.foPct = (int)lroundf(hf);
  } else {
    JsonObjectConst pbgs = doc["playerByGameStats"].as<JsonObjectConst>();
    if (!pbgs.isNull()) {
      JsonObjectConst awayTeam = pbgs["awayTeam"].as<JsonObjectConst>();
      JsonObjectConst homeTeam = pbgs["homeTeam"].as<JsonObjectConst>();
      if (!awayTeam.isNull()) io.away.hits = sumHitsFromTeam(awayTeam);
      if (!homeTeam.isNull()) io.home.hits = sumHitsFromTeam(homeTeam);
    }
  }

  return true;
}

bool NhlClient::fetchPregameTicker(GameState &io, const String &focusTeamAbbr) {
  if (!io.hasGame || io.gameId.isEmpty()) return false;

  JsonDocument doc;
  if (!httpGetJson(String(kBase) + "/gamecenter/" + io.gameId + "/boxscore", doc)) return false;

  JsonObjectConst pbgs = doc["playerByGameStats"].as<JsonObjectConst>();
  if (pbgs.isNull()) {
    io.pregameTicker = "LINEUP TBA";
    return true;
  }

  String focus = focusTeamAbbr;
  focus.toUpperCase();
  String home = io.home.abbr;
  String away = io.away.abbr;
  home.toUpperCase();
  away.toUpperCase();

  bool focusIsHome = (home == focus);
  if (!focusIsHome && away != focus) {
    io.pregameTicker = "LINEUP TBA";
    return true;
  }

  JsonObjectConst team = pbgs[focusIsHome ? "homeTeam" : "awayTeam"].as<JsonObjectConst>();
  if (team.isNull()) {
    io.pregameTicker = "LINEUP TBA";
    return true;
  }

  String names;
  uint8_t nameCount = 0;
  appendTickerNames(team["forwards"].as<JsonArrayConst>(), names, nameCount, 24);
  appendTickerNames(team["defense"].as<JsonArrayConst>(), names, nameCount, 24);
  appendTickerNames(team["goalies"].as<JsonArrayConst>(), names, nameCount, 24);

  if (!names.length()) {
    io.pregameTicker = "LINEUP TBA";
    return true;
  }

  io.pregameTicker = focus + " LINEUP: " + names;
  return true;
}

bool NhlClient::fetchLatestGoal(GameState &io, const String &focusTeamAbbr) {
  if (!io.hasGame || io.gameId.isEmpty()) return false;
  io.hasPenaltyCountdown = false;
  io.penaltyCountdown = "";
  io.strengthLabel = "EVEN STRENGTH";

  JsonDocument doc;
  if (!httpGetJson(String(kBase) + "/gamecenter/" + io.gameId + "/play-by-play", doc)) return false;

  const int homeId = doc["homeTeam"]["id"] | 0;
  const int awayId = doc["awayTeam"]["id"] | 0;
  const String homeAbbr = jsonStringOrDefault(doc["homeTeam"]["abbrev"]);
  const String awayAbbr = jsonStringOrDefault(doc["awayTeam"]["abbrev"]);

  JsonArray plays = doc["plays"].as<JsonArray>();
  if (plays.isNull() || plays.size() == 0) return false;

  String situation = "";
  for (int i = (int)plays.size() - 1; i >= 0; --i) {
    const char *code_c = plays[i]["situationCode"] | "";
    if (code_c && code_c[0]) {
      situation = code_c;
      break;
    }
  }
  if (situation.length()) {
    applyStrengthFromSituation(io, situation, homeAbbr, awayAbbr);
  }

  // Derive a small PP countdown from on-ice penalty events.
  static const uint8_t kMaxTrackedPenalties = 16;
  ActivePenalty penalties[kMaxTrackedPenalties];
  uint8_t penaltyCount = 0;

  auto addPenalty = [&](bool onHome, int startElapsed, int durationSec, bool releasableOnPpGoal) {
    if (durationSec <= 0) return;
    if (penaltyCount >= kMaxTrackedPenalties) return;
    ActivePenalty &p = penalties[penaltyCount++];
    p.active = true;
    p.onHome = onHome;
    p.startElapsed = startElapsed;
    p.durationSec = durationSec;
    p.releasableOnPpGoal = releasableOnPpGoal;
  };

  auto releaseOneMinor = [&](bool onHome, int atElapsed) {
    int bestIdx = -1;
    int bestStart = INT_MAX;
    for (uint8_t i = 0; i < penaltyCount; ++i) {
      ActivePenalty &p = penalties[i];
      if (!p.active) continue;
      if (p.onHome != onHome) continue;
      if (!p.releasableOnPpGoal) continue;
      const int endElapsed = p.startElapsed + p.durationSec;
      if (atElapsed < p.startElapsed || atElapsed >= endElapsed) continue;
      if (p.startElapsed < bestStart) {
        bestStart = p.startElapsed;
        bestIdx = i;
      }
    }
    if (bestIdx >= 0) penalties[bestIdx].active = false;
  };

  for (JsonObject p : plays) {
    const char *type_c = p["typeDescKey"] | "";
    const String type = type_c ? type_c : "";

    const uint8_t periodNum = (uint8_t)(p["periodDescriptor"]["number"] | 0);
    const char *periodType_c = p["periodDescriptor"]["periodType"] | "";
    const String periodType = periodType_c ? periodType_c : "";
    const char *timeInPeriod_c = p["timeInPeriod"] | "";
    const String timeInPeriod = timeInPeriod_c ? timeInPeriod_c : "";
    const int playElapsed = elapsedFromTimeInPeriod(periodNum, periodType, timeInPeriod, io.isPlayoff);

    if (type == "penalty") {
      if (playElapsed < 0) continue;
      const int durationMin = p["details"]["duration"] | 0;
      const char *penType_c = p["details"]["typeCode"] | "";
      String penType = penType_c ? penType_c : "";
      penType.toUpperCase();

      // Ignore penalties that do not create a skater disadvantage.
      if (durationMin <= 0 || durationMin >= 10) continue;
      if (penType == "MIS" || penType == "GAM" || penType == "MAT") continue;

      const int teamId = p["details"]["eventOwnerTeamId"] | 0;
      bool onHome = false;
      if (!teamIdToHomeFlag(teamId, homeId, awayId, onHome)) continue;

      if (durationMin == 4 || penType == "DBL") {
        // First-pass handling for double minors: two stacked 2-minute segments.
        addPenalty(onHome, playElapsed, 120, true);
        addPenalty(onHome, playElapsed + 120, 120, true);
      } else {
        const bool releasable = (durationMin <= 2);
        addPenalty(onHome, playElapsed, durationMin * 60, releasable);
      }
      continue;
    }

    if (type == "goal") {
      if (playElapsed < 0) continue;

      int scoringTeamId = p["details"]["eventOwnerTeamId"] | 0;
      if (!scoringTeamId) scoringTeamId = p["details"]["scoringTeamId"] | 0;
      bool scoringOnHome = false;
      if (!teamIdToHomeFlag(scoringTeamId, homeId, awayId, scoringOnHome)) continue;

      const char *goalCode_c = p["situationCode"] | "";
      const String goalCode = goalCode_c ? goalCode_c : "";
      int awaySkaters = 0;
      int homeSkaters = 0;
      bool goaliesPresent = false;
      if (!parseSituationCode(goalCode, awaySkaters, homeSkaters, goaliesPresent)) continue;
      if (!goaliesPresent || awaySkaters == homeSkaters) continue;

      // If the scoring team has the skater advantage, release one active minor.
      const bool scoringHadAdvantage = scoringOnHome ? (homeSkaters > awaySkaters)
                                                     : (awaySkaters > homeSkaters);
      if (scoringHadAdvantage) {
        const bool penalizedOnHome = !scoringOnHome;
        releaseOneMinor(penalizedOnHome, playElapsed);
      }
    }
  }

  int awaySkaters = 0;
  int homeSkaters = 0;
  bool goaliesPresent = false;
  if (parseSituationCode(situation, awaySkaters, homeSkaters, goaliesPresent) &&
      goaliesPresent && awaySkaters != homeSkaters) {
    const uint8_t currentPeriodNum = (uint8_t)(doc["periodDescriptor"]["number"] | io.period);
    const char *currentPeriodType_c = doc["periodDescriptor"]["periodType"] | "";
    const String currentPeriodType = currentPeriodType_c ? currentPeriodType_c : io.periodType;

    int secondsRemaining = doc["clock"]["secondsRemaining"] | -1;
    if (secondsRemaining < 0) {
      const char *clock_c = doc["clock"]["timeRemaining"] | "";
      const String clock = clock_c ? clock_c : "";
      secondsRemaining = parseClockToSeconds(clock);
    }

    const int currentElapsed = elapsedFromCurrentClock(currentPeriodNum, currentPeriodType, secondsRemaining, io.isPlayoff);
    if (currentElapsed >= 0) {
      const bool penalizedOnHome = (homeSkaters < awaySkaters);
      int bestRemaining = INT_MAX;
      for (uint8_t i = 0; i < penaltyCount; ++i) {
        const ActivePenalty &p = penalties[i];
        if (!p.active) continue;
        if (p.onHome != penalizedOnHome) continue;
        if (currentElapsed < p.startElapsed) continue;
        const int endElapsed = p.startElapsed + p.durationSec;
        const int remaining = endElapsed - currentElapsed;
        if (remaining > 0 && remaining < bestRemaining) {
          bestRemaining = remaining;
        }
      }
      if (bestRemaining != INT_MAX) {
        io.hasPenaltyCountdown = true;
        io.penaltyCountdown = formatPenaltyCountdown(bestRemaining);
      }
    }
  }

  int homeWins = 0;
  int awayWins = 0;
  int totalFaceoffs = 0;
  for (JsonObject p : plays) {
    const char *type_c = p["typeDescKey"] | "";
    if (type_c && strcmp(type_c, "faceoff") == 0) {
      const int ownerId = p["details"]["eventOwnerTeamId"] | 0;
      if (ownerId == homeId) {
        homeWins++;
        totalFaceoffs++;
      } else if (ownerId == awayId) {
        awayWins++;
        totalFaceoffs++;
      }
    }
  }
  if (totalFaceoffs > 0) {
    io.home.foPct = (int)lroundf((homeWins * 100.0f) / (float)totalFaceoffs);
    io.away.foPct = (int)lroundf((awayWins * 100.0f) / (float)totalFaceoffs);
  }

  for (int i = (int)plays.size() - 1; i >= 0; --i) {
    JsonObject p = plays[i];
    const char *type_c = p["typeDescKey"] | "";
    String type = type_c ? type_c : "";
    if (type != "goal") continue;

    uint32_t eventId = p["eventId"] | 0;
    if (!eventId) return false;

    String owner = jsonStringOrDefault(p["details"]["eventOwnerTeamAbbrev"]);
    if (owner.isEmpty()) owner = jsonStringOrDefault(p["details"]["teamAbbrev"]);
    if (owner.isEmpty()) owner = jsonStringOrDefault(p["details"]["teamTricode"]);
    if (owner.isEmpty()) {
      const int ownerId = p["details"]["eventOwnerTeamId"] | 0;
      if (ownerId && ownerId == homeId) owner = homeAbbr;
      if (ownerId && ownerId == awayId) owner = awayAbbr;
    }
    if (owner.isEmpty()) {
      const int scoringTeamId = p["details"]["scoringTeamId"] | 0;
      if (scoringTeamId && scoringTeamId == homeId) owner = homeAbbr;
      if (scoringTeamId && scoringTeamId == awayId) owner = awayAbbr;
    }
    io.leafsJustScored = (owner == focusTeamAbbr);
    io.goalTeamAbbr = owner;

    const char *scorer_c = p["details"]["scoringPlayerName"] | "";
    const char *a1_c = p["details"]["assist1PlayerName"] | "";
    const char *a2_c = p["details"]["assist2PlayerName"] | "";
    String scorer = scorer_c ? scorer_c : "";
    String a1 = a1_c ? a1_c : "";
    String a2 = a2_c ? a2_c : "";

    String line;
    if (a1.length() || a2.length()) {
      line = "ASSISTS: ";
      if (a1.length()) line += a1;
      if (a2.length()) { if (a1.length()) line += ", "; line += a2; }
    }

    io.goalText = line;
    io.goalScorer = scorer;
    io.lastGoalEventId = eventId;
    return true;
  }

  return false;
}

bool NhlClient::fetchNextGame(GameState &io, const String &focusTeamAbbr) {
  JsonDocument doc;
  const String url = String(kBase) + "/club-schedule/" + focusTeamAbbr + "/week/now";
  if (!httpGetJson(url, doc)) return false;

  JsonArray games = doc["games"].as<JsonArray>();
  if (games.isNull()) {
    io.hasNextGame = false;
    io.nextOppAbbr = "";
    io.nextIsHome = false;
    io.nextIsPlayoff = false;
    io.nextVenue = "";
    io.nextCity = "";
    io.nextStartEpoch = 0;
    return true;
  }

  time_t bestEpoch = 0;
  String bestOpp;
  bool bestIsHome = false;
  bool bestIsPlayoff = false;
  String bestVenue;
  String bestCity;

  for (JsonObject g : games) {
    const char *state_c = g["gameState"] | "";
    const String state = state_c ? state_c : "";

    // We only want upcoming games.
    if (state != "FUT" && state != "PRE") continue;

    const char *homeAbbr_c = g["homeTeam"]["abbrev"] | "";
    const char *awayAbbr_c = g["awayTeam"]["abbrev"] | "";
    const String homeAbbr = homeAbbr_c ? homeAbbr_c : "";
    const String awayAbbr = awayAbbr_c ? awayAbbr_c : "";

    if (homeAbbr != focusTeamAbbr && awayAbbr != focusTeamAbbr) continue;

    const char *start_c = g["startTimeUTC"] | "";
    const String startIso = start_c ? start_c : "";
    time_t epoch = 0;
    if (!parseIsoUtcToEpoch(startIso, epoch)) continue;

    if (bestEpoch == 0 || epoch < bestEpoch) {
      const int gameType = g["gameType"] | 0;
      bestEpoch = epoch;
      bestIsHome = (homeAbbr == focusTeamAbbr);
      bestIsPlayoff = (gameType == 3);
      bestOpp = bestIsHome ? awayAbbr : homeAbbr;
      bestVenue = (const char *)(g["venue"]["default"] | "");
      // Use the HOME team's place name as the city context.
      bestCity = (const char *)(g["homeTeam"]["placeName"]["default"] | "");
    }
  }

  if (bestEpoch == 0 || bestOpp.isEmpty()) {
    io.hasNextGame = false;
    io.nextOppAbbr = "";
    io.nextIsHome = false;
    io.nextIsPlayoff = false;
    io.nextVenue = "";
    io.nextCity = "";
    io.nextStartEpoch = 0;
    return true;
  }

  io.hasNextGame = true;
  io.nextOppAbbr = bestOpp;
  io.nextIsHome = bestIsHome;
  io.nextIsPlayoff = bestIsPlayoff;
  io.nextVenue = bestVenue;
  io.nextCity = bestCity;
  io.nextStartEpoch = bestEpoch;
  return true;
}

bool NhlClient::fetchLastGameRecap(GameState &io, const String &focusTeamAbbr) {
  LastGameRecap recap;
  clearRecap(recap);

  JsonDocument scheduleFilter;
  scheduleFilter["previousMonth"] = true;
  scheduleFilter["games"][0]["id"] = true;
  scheduleFilter["games"][0]["gameState"] = true;
  scheduleFilter["games"][0]["startTimeUTC"] = true;
  scheduleFilter["games"][0]["awayTeam"]["abbrev"] = true;
  scheduleFilter["games"][0]["awayTeam"]["score"] = true;
  scheduleFilter["games"][0]["homeTeam"]["abbrev"] = true;
  scheduleFilter["games"][0]["homeTeam"]["score"] = true;
  scheduleFilter["games"][0]["venue"]["default"] = true;
  scheduleFilter["games"][0]["homeTeam"]["placeName"]["default"] = true;

  auto applySchedule = [&](JsonDocument &doc) -> bool {
    JsonArray games = doc["games"].as<JsonArray>();
    if (games.isNull()) return false;

    time_t bestEpoch = 0;
    JsonObject best;
    for (JsonObject g : games) {
      const char *state_c = g["gameState"] | "";
      const String state = state_c ? state_c : "";
      if (state != "FINAL" && state != "OFF") continue;

      const String homeAbbr = String((const char *)(g["homeTeam"]["abbrev"] | ""));
      const String awayAbbr = String((const char *)(g["awayTeam"]["abbrev"] | ""));
      if (homeAbbr != focusTeamAbbr && awayAbbr != focusTeamAbbr) continue;

      const char *start_c = g["startTimeUTC"] | "";
      const String startIso = start_c ? start_c : "";
      time_t epoch = 0;
      if (!parseIsoUtcToEpoch(startIso, epoch)) continue;
      if (bestEpoch == 0 || epoch > bestEpoch) {
        bestEpoch = epoch;
        best = g;
      }
    }

    if (bestEpoch == 0) return false;

    recap.hasGame = true;
    recap.gameId = String((int)(best["id"] | 0));
    recap.startEpoch = bestEpoch;
    recap.home.abbr = String((const char *)(best["homeTeam"]["abbrev"] | ""));
    recap.away.abbr = String((const char *)(best["awayTeam"]["abbrev"] | ""));
    recap.home.score = best["homeTeam"]["score"] | 0;
    recap.away.score = best["awayTeam"]["score"] | 0;
    recap.venue = String((const char *)(best["venue"]["default"] | ""));
    recap.city = String((const char *)(best["homeTeam"]["placeName"]["default"] | ""));
    return true;
  };

  JsonDocument doc;
  String scheduleUrl = String(kBase) + "/club-schedule/" + focusTeamAbbr + "/month/now";
  bool scheduleOk = httpGetJson(scheduleUrl, doc, scheduleFilter);
  if (!scheduleOk) {
    doc.clear();
    scheduleOk = httpGetJson(scheduleUrl, doc);
  }
  if (scheduleOk && !applySchedule(doc)) {
    const String prevMonth = doc["previousMonth"] | "";
    if (prevMonth.length()) {
      doc.clear();
      scheduleUrl = String(kBase) + "/club-schedule/" + focusTeamAbbr + "/month/" + prevMonth;
      bool prevOk = httpGetJson(scheduleUrl, doc, scheduleFilter);
      if (!prevOk) {
        doc.clear();
        prevOk = httpGetJson(scheduleUrl, doc);
      }
      if (prevOk) {
        applySchedule(doc);
      }
    }
  }

  if (!scheduleOk) return false;
  if (!recap.hasGame) {
    io.last = recap;
    return true;
  }

  // Fetch scoring summary for scorers and period goals.
  JsonDocument landingFilter;
  landingFilter["awayTeam"]["abbrev"] = true;
  landingFilter["awayTeam"]["score"] = true;
  landingFilter["homeTeam"]["abbrev"] = true;
  landingFilter["homeTeam"]["score"] = true;
  landingFilter["summary"]["scoring"][0]["periodDescriptor"]["number"] = true;
  landingFilter["summary"]["scoring"][0]["periodDescriptor"]["periodType"] = true;
  landingFilter["summary"]["scoring"][0]["goals"][0]["teamAbbrev"]["default"] = true;
  landingFilter["summary"]["scoring"][0]["goals"][0]["teamAbbrev"] = true;
  landingFilter["summary"]["scoring"][0]["goals"][0]["lastName"]["default"] = true;
  landingFilter["summary"]["scoring"][0]["goals"][0]["lastName"] = true;
  landingFilter["summary"]["scoring"][0]["goals"][0]["name"]["default"] = true;
  landingFilter["summary"]["scoring"][0]["goals"][0]["name"] = true;

  const String landingUrl = String(kBase) + "/gamecenter/" + recap.gameId + "/landing";
  JsonDocument landing;
  bool landingOk = httpGetJson(landingUrl, landing, landingFilter);
  if (!landingOk) {
    landing.clear();
    landingOk = httpGetJson(landingUrl, landing);
  }
  if (landingOk) {
    recap.home.abbr = String((const char *)(landing["homeTeam"]["abbrev"] | recap.home.abbr.c_str()));
    recap.away.abbr = String((const char *)(landing["awayTeam"]["abbrev"] | recap.away.abbr.c_str()));
    recap.home.score = landing["homeTeam"]["score"] | recap.home.score;
    recap.away.score = landing["awayTeam"]["score"] | recap.away.score;

    JsonArray periods = landing["summary"]["scoring"].as<JsonArray>();
    if (!periods.isNull()) {
      for (JsonObject p : periods) {
        if (recap.periodCount >= kRecapMaxPeriods) break;
        PeriodEntry &entry = recap.periods[recap.periodCount++];
        entry.label = periodLabelFromDescriptor(p["periodDescriptor"].as<JsonObject>());
        entry.home = 0;
        entry.away = 0;

        JsonArray goals = p["goals"].as<JsonArray>();
        for (JsonObject g : goals) {
          const String team = jsonStringOrDefault(g["teamAbbrev"]);
          const String lastName = jsonStringOrDefault(g["lastName"]);
          String name = lastName;
          if (name.isEmpty()) name = jsonStringOrDefault(g["name"]);

          if (team == recap.home.abbr) {
            entry.home++;
            addScorer(recap.homeScorers, recap.homeScorerCount, name);
          } else if (team == recap.away.abbr) {
            entry.away++;
            addScorer(recap.awayScorers, recap.awayScorerCount, name);
          }
        }
      }
    }
  }

  io.last = recap;
  return true;
}

bool NhlClient::fetchConferenceStandings(GameState &io, const String &focusTeamAbbr) {
  ConferenceStandings out;
  out.hasData = false;
  out.teamCount = 0;
  out.focusIndex = 255;

  JsonDocument filter;
  filter["standings"][0]["conferenceAbbrev"] = true;
  filter["standings"][0]["conferenceName"] = true;
  filter["standings"][0]["conferenceSequence"] = true;
  filter["standings"][0]["divisionAbbrev"] = true;
  filter["standings"][0]["divisionName"] = true;
  filter["standings"][0]["divisionSequence"] = true;
  filter["standings"][0]["teamAbbrev"]["default"] = true;
  filter["standings"][0]["gamesPlayed"] = true;
  filter["standings"][0]["points"] = true;
  filter["standings"][0]["wins"] = true;
  filter["standings"][0]["losses"] = true;
  filter["standings"][0]["otLosses"] = true;
  filter["standings"][0]["l10Wins"] = true;
  filter["standings"][0]["l10Losses"] = true;
  filter["standings"][0]["l10OtLosses"] = true;
  filter["standings"][0]["streakCode"] = true;
  filter["standings"][0]["streakCount"] = true;

  JsonDocument doc;
  const String url = String(kBase) + "/standings/now";
  if (!httpGetJson(url, doc, filter)) {
    doc.clear();
    if (!httpGetJson(url, doc)) return false;
  }

  JsonArray standings = doc["standings"].as<JsonArray>();
  if (standings.isNull()) {
    io.standings = out;
    return true;
  }

  String focus = focusTeamAbbr;
  focus.toUpperCase();

  String targetConference;
  for (JsonObject s : standings) {
    String abbr = jsonStringOrDefault(s["teamAbbrev"]);
    abbr.toUpperCase();
    if (abbr == focus) {
      const char *conf_c = s["conferenceAbbrev"] | "";
      targetConference = conf_c ? conf_c : "";
      const char *name_c = s["conferenceName"] | "";
      out.conferenceName = name_c ? name_c : "";
      break;
    }
  }

  if (targetConference.isEmpty()) {
    io.standings = out;
    return true;
  }

  out.conferenceAbbrev = targetConference;

  for (JsonObject s : standings) {
    const char *conf_c = s["conferenceAbbrev"] | "";
    const String conf = conf_c ? conf_c : "";
    if (conf != targetConference) continue;
    if (out.teamCount >= kConferenceStandingsMaxTeams) continue;

    ConferenceStandingEntry &e = out.teams[out.teamCount];
    int rank = s["conferenceSequence"] | 0;
    if (rank < 0) rank = 0;
    if (rank > 255) rank = 255;
    e.rank = (uint8_t)rank;
    int divisionRank = s["divisionSequence"] | 0;
    if (divisionRank < 0) divisionRank = 0;
    if (divisionRank > 255) divisionRank = 255;
    e.divisionRank = (uint8_t)divisionRank;
    e.abbr = jsonStringOrDefault(s["teamAbbrev"]);
    e.divisionAbbrev = String((const char *)(s["divisionAbbrev"] | ""));
    e.divisionName = String((const char *)(s["divisionName"] | ""));

    int gp = s["gamesPlayed"] | 0;
    if (gp < 0) gp = 0;
    if (gp > 255) gp = 255;
    e.gamesPlayed = (uint8_t)gp;

    int pts = s["points"] | 0;
    if (pts < 0) pts = 0;
    if (pts > 65535) pts = 65535;
    e.points = (uint16_t)pts;

    int wins = s["wins"] | 0;
    if (wins < 0) wins = 0;
    if (wins > 255) wins = 255;
    e.wins = (uint8_t)wins;

    int losses = s["losses"] | 0;
    if (losses < 0) losses = 0;
    if (losses > 255) losses = 255;
    e.losses = (uint8_t)losses;

    int otl = s["otLosses"] | 0;
    if (otl < 0) otl = 0;
    if (otl > 255) otl = 255;
    e.otLosses = (uint8_t)otl;

    int l10w = s["l10Wins"] | 0;
    if (l10w < 0) l10w = 0;
    if (l10w > 255) l10w = 255;
    e.l10Wins = (uint8_t)l10w;

    int l10l = s["l10Losses"] | 0;
    if (l10l < 0) l10l = 0;
    if (l10l > 255) l10l = 255;
    e.l10Losses = (uint8_t)l10l;

    int l10otl = s["l10OtLosses"] | 0;
    if (l10otl < 0) l10otl = 0;
    if (l10otl > 255) l10otl = 255;
    e.l10OtLosses = (uint8_t)l10otl;

    const char *streak_c = s["streakCode"] | "";
    e.streakCode = (streak_c && streak_c[0]) ? streak_c[0] : '-';
    int streakCount = s["streakCount"] | 0;
    if (streakCount < 0) streakCount = 0;
    if (streakCount > 255) streakCount = 255;
    e.streakCount = (uint8_t)streakCount;

    String abbr = e.abbr;
    abbr.toUpperCase();
    if (abbr == focus) out.focusIndex = out.teamCount;

    out.teamCount++;
  }

  // Ensure conference rows are in conference-rank order.
  for (uint8_t i = 0; i < out.teamCount; ++i) {
    uint8_t best = i;
    for (uint8_t j = (uint8_t)(i + 1); j < out.teamCount; ++j) {
      if (out.teams[j].rank > 0 &&
          (out.teams[best].rank == 0 || out.teams[j].rank < out.teams[best].rank)) {
        best = j;
      }
    }
    if (best != i) {
      ConferenceStandingEntry tmp = out.teams[i];
      out.teams[i] = out.teams[best];
      out.teams[best] = tmp;
      if (out.focusIndex == i) out.focusIndex = best;
      else if (out.focusIndex == best) out.focusIndex = i;
    }
  }

  out.hasData = (out.teamCount > 0);
  io.standings = out;
  return true;
}
