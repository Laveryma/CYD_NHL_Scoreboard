#include "anthem.h"

#include <SPIFFS.h>

#include "config.h"

namespace {

static const char *kAnthemPath = "/audio/anthem.wav";

static bool sPrimed = false;
static String sLastEventId;
static String sLastState;
static String sLastPlayedEventId;

#ifndef ANTHEM_DAC_PIN
#define ANTHEM_DAC_PIN 25
#endif
#ifndef ANTHEM_DAC_PIN_ALT
#define ANTHEM_DAC_PIN_ALT -1
#endif
#ifndef ANTHEM_GAIN_PCT
#define ANTHEM_GAIN_PCT 100
#endif

static constexpr bool isValidDacPin(int pin) {
  return (pin == 25 || pin == 26);
}

static constexpr int kPrimaryDacPin = isValidDacPin(ANTHEM_DAC_PIN) ? ANTHEM_DAC_PIN : 25;
static constexpr int kAltDacPin =
  (isValidDacPin(ANTHEM_DAC_PIN_ALT) && ANTHEM_DAC_PIN_ALT != kPrimaryDacPin) ? ANTHEM_DAC_PIN_ALT : -1;

static int16_t configuredGainPct() {
  int16_t gain = (int16_t)ANTHEM_GAIN_PCT;
  if (gain < 0) gain = 0;
  if (gain > 200) gain = 200;
  return gain;
}

static String stateFromGame(const GameState &g) {
  if (!g.hasGame || g.gameId.isEmpty()) return String("none");
  if (g.isLive || g.isIntermission) return String("in");
  if (g.isPre) return String("pre");
  if (g.isFinal) return String("post");
  return String("other");
}

static bool readU16(File &f, uint16_t &out) {
  uint8_t b[2];
  if (f.read(b, 2) != 2) return false;
  out = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
  return true;
}

static bool readU32(File &f, uint32_t &out) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return false;
  out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

static bool seekAhead(File &f, uint32_t n) {
  const uint32_t pos = (uint32_t)f.position();
  return f.seek(pos + n);
}

static bool parseWavHeader(File &f,
                           uint32_t &sampleRate,
                           uint16_t &channels,
                           uint16_t &bitsPerSample,
                           uint32_t &dataOffset,
                           uint32_t &dataSize) {
  char riff[4];
  if (f.readBytes(riff, 4) != 4) return false;
  if (memcmp(riff, "RIFF", 4) != 0) return false;

  uint32_t riffSize = 0;
  if (!readU32(f, riffSize)) return false;
  (void)riffSize;

  char wave[4];
  if (f.readBytes(wave, 4) != 4) return false;
  if (memcmp(wave, "WAVE", 4) != 0) return false;

  bool fmtFound = false;
  bool dataFound = false;
  sampleRate = 0;
  channels = 0;
  bitsPerSample = 0;
  dataOffset = 0;
  dataSize = 0;

  while (f.available()) {
    char chunkId[4];
    if (f.readBytes(chunkId, 4) != 4) break;

    uint32_t chunkSize = 0;
    if (!readU32(f, chunkSize)) return false;

    if (memcmp(chunkId, "fmt ", 4) == 0) {
      uint16_t audioFormat = 0;
      uint16_t blockAlign = 0;
      uint32_t byteRate = 0;
      if (!readU16(f, audioFormat)) return false;
      if (!readU16(f, channels)) return false;
      if (!readU32(f, sampleRate)) return false;
      if (!readU32(f, byteRate)) return false;
      if (!readU16(f, blockAlign)) return false;
      if (!readU16(f, bitsPerSample)) return false;
      (void)byteRate;
      (void)blockAlign;

      if (chunkSize > 16) {
        if (!seekAhead(f, chunkSize - 16)) return false;
      }

      if (audioFormat != 1) {
        return false;
      }
      fmtFound = true;
    } else if (memcmp(chunkId, "data", 4) == 0) {
      dataOffset = (uint32_t)f.position();
      dataSize = chunkSize;
      if (!seekAhead(f, chunkSize)) return false;
      dataFound = true;
    } else {
      if (!seekAhead(f, chunkSize)) return false;
    }

    if (chunkSize & 1U) {
      if (!seekAhead(f, 1)) return false;
    }

    if (fmtFound && dataFound) break;
  }

  return fmtFound && dataFound && sampleRate > 0;
}

static uint8_t applyGainU8(uint8_t in, int16_t gainPct) {
  int32_t centered = (int32_t)in - 128;
  centered = (centered * (int32_t)gainPct) / 100;
  if (centered > 127) centered = 127;
  if (centered < -128) centered = -128;
  return (uint8_t)(centered + 128);
}

static int16_t applyGainS16(int16_t in, int16_t gainPct) {
  int32_t scaled = ((int32_t)in * (int32_t)gainPct) / 100;
  if (scaled > 32767) scaled = 32767;
  if (scaled < -32768) scaled = -32768;
  return (int16_t)scaled;
}

struct BootButtonDebounce {
  bool lastRead = true;
  bool stable = true;
  uint32_t lastChangeMs = 0;
};

static void initBootButtonDebounce(BootButtonDebounce &btn) {
  const bool read = (digitalRead(BOOT_BTN_PIN) == HIGH);
  btn.lastRead = read;
  btn.stable = read;
  btn.lastChangeMs = millis();
}

static bool pollBootClick(BootButtonDebounce &btn, uint32_t nowMs) {
  const bool read = (digitalRead(BOOT_BTN_PIN) == HIGH);
  if (read != btn.lastRead) {
    btn.lastRead = read;
    btn.lastChangeMs = nowMs;
  }
  if ((nowMs - btn.lastChangeMs) < 35) return false;
  if (read != btn.stable) {
    btn.stable = read;
    // BOOT is active-low with INPUT_PULLUP; treat a press edge as one click.
    return !btn.stable;
  }
  return false;
}

static void writeDacSample(uint8_t out) {
  dacWrite(kPrimaryDacPin, out);
  if (kAltDacPin >= 0) {
    dacWrite(kAltDacPin, out);
  }
}

static void enableDacOutputs() {
  pinMode(kPrimaryDacPin, OUTPUT);
  if (kAltDacPin >= 0) {
    pinMode(kAltDacPin, OUTPUT);
  }
}

static void muteAndDisableDacOutputs() {
  // Return DAC to midscale briefly to avoid edge pops, then disable output drivers.
  writeDacSample(128);
  delay(2);
  dacDisable(kPrimaryDacPin);
  pinMode(kPrimaryDacPin, INPUT);
  if (kAltDacPin >= 0) {
    dacDisable(kAltDacPin);
    pinMode(kAltDacPin, INPUT);
  }
}

}  // namespace

namespace Anthem {

void begin() {
  if (!isValidDacPin(ANTHEM_DAC_PIN)) {
    Serial.printf("ANTHEM: invalid ANTHEM_DAC_PIN=%d, using %d\n", (int)ANTHEM_DAC_PIN, (int)kPrimaryDacPin);
  }
  if (ANTHEM_DAC_PIN_ALT != -1 && kAltDacPin < 0) {
    Serial.printf("ANTHEM: ignoring ANTHEM_DAC_PIN_ALT=%d\n", (int)ANTHEM_DAC_PIN_ALT);
  }
  if (!SPIFFS.begin(false)) {
    // Assets::begin() already mounts SPIFFS with format-on-fail.
    // Keep this non-destructive; if not mounted yet it will be handled there.
  }
}

void prime(const GameState &g) {
  sLastEventId = g.gameId;
  sLastState = stateFromGame(g);
  sPrimed = true;
}

bool playNow() {
  if (!SPIFFS.begin(false)) {
    Serial.println("ANTHEM: SPIFFS not mounted");
    return false;
  }
  if (!SPIFFS.exists(kAnthemPath)) {
    Serial.println("ANTHEM: /audio/anthem.wav not found");
    return false;
  }

  File f = SPIFFS.open(kAnthemPath, "r");
  if (!f) {
    Serial.println("ANTHEM: failed to open WAV");
    return false;
  }

  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  if (!parseWavHeader(f, sampleRate, channels, bitsPerSample, dataOffset, dataSize)) {
    Serial.println("ANTHEM: invalid WAV header");
    f.close();
    return false;
  }

  if (channels != 1 || (bitsPerSample != 16 && bitsPerSample != 8)) {
    Serial.printf("ANTHEM: unsupported format channels=%u bits=%u\n", channels, bitsPerSample);
    f.close();
    return false;
  }

  const int16_t configuredGain = configuredGainPct();
  if (configuredGain != (int16_t)ANTHEM_GAIN_PCT) {
    Serial.printf("ANTHEM: gain clamped from %d%% to %d%%\n", (int)ANTHEM_GAIN_PCT, (int)configuredGain);
  }

  Serial.printf("ANTHEM: sr=%luHz ch=%u bits=%u gain=%d%% pin=%d alt=%d\n",
                (unsigned long)sampleRate,
                (unsigned)channels,
                (unsigned)bitsPerSample,
                (int)configuredGain,
                (int)kPrimaryDacPin,
                (int)kAltDacPin);

  if (!f.seek(dataOffset)) {
    Serial.println("ANTHEM: seek failed");
    f.close();
    return false;
  }

  const uint32_t samplePeriodUs = (sampleRate == 0) ? 0 : (1000000UL / sampleRate);
  if (!samplePeriodUs) {
    f.close();
    return false;
  }

  enableDacOutputs();
  int16_t activeGainPct = configuredGain;
  BootButtonDebounce bootBtn;
  initBootButtonDebounce(bootBtn);

  uint32_t played = 0;
  uint32_t sampleCount = 0;
  uint32_t nextUs = micros();

  while (played < dataSize && f.available()) {
    uint8_t out = 128;
    if (bitsPerSample == 16) {
      if (played + 1 >= dataSize || f.available() < 2) break;
      uint16_t raw = 0;
      if (!readU16(f, raw)) break;
      played += 2;

      const int16_t sample = applyGainS16((int16_t)raw, activeGainPct);
      out = (uint8_t)(((int32_t)sample + 32768) >> 8);
    } else {
      const int raw8 = f.read();
      if (raw8 < 0) break;
      played += 1;
      // 8-bit PCM WAV uses unsigned samples 0..255, which maps directly to the ESP32 DAC range.
      out = applyGainU8((uint8_t)raw8, activeGainPct);
    }

    while ((int32_t)(micros() - nextUs) < 0) {
      delayMicroseconds(20);
    }
    writeDacSample(out);
    nextUs += samplePeriodUs;

    sampleCount++;
    if ((sampleCount & 0x0FU) == 0U) {
      const uint32_t nowMs = millis();
      if (pollBootClick(bootBtn, nowMs)) {
        if (activeGainPct > 0) {
          activeGainPct -= 10;
          if (activeGainPct < 0) activeGainPct = 0;
        }
        Serial.printf("ANTHEM: BOOT click -> gain=%d%%\n", (int)activeGainPct);
      }
    }
    if ((sampleCount % 1024U) == 0U) {
      yield();
    }
  }

  muteAndDisableDacOutputs();
  f.close();
  Serial.println("ANTHEM: playback complete, DAC disabled");
  return true;
}

void tick(const GameState &g) {
  const String currentState = stateFromGame(g);
  const String currentEventId = g.gameId;

  if (!sPrimed) {
    prime(g);
    return;
  }

  if (currentEventId.isEmpty()) {
    sLastEventId = "";
    sLastState = currentState;
    return;
  }

  if (currentEventId != sLastEventId) {
    sLastEventId = currentEventId;
    sLastState = currentState;
    return;
  }

  const bool preToIn = (sLastState == "pre" && currentState == "in");
  if (preToIn && sLastPlayedEventId != currentEventId) {
    if (playNow()) {
      sLastPlayedEventId = currentEventId;
    }
  }

  sLastState = currentState;
}

}  // namespace Anthem
