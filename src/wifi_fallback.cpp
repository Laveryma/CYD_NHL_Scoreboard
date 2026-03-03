#include <WiFi.h>
#include "config.h"
#include "wifi_fallback.h"

struct WifiCred {
  const char* ssid;
  const char* pass;
};

static String lastConnectedSsid;
static int32_t lastConnectedRssi = -127;

#ifndef WIFI_RSSI_HYST_DB
#define WIFI_RSSI_HYST_DB 6
#endif

static bool tryConnect(const WifiCred& c, uint32_t timeoutMs) {
  if (!c.ssid || c.ssid[0] == '\0') return false;

  Serial.print("Wi-Fi: connecting to ");
  Serial.println(c.ssid);

  WiFi.begin(c.ssid, c.pass);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi: connected to ");
    Serial.print(c.ssid);
    Serial.print(" | IP ");
    Serial.println(WiFi.localIP());
    lastConnectedSsid = String(c.ssid);
    lastConnectedRssi = WiFi.RSSI();
    return true;
  }

  Serial.println("Wi-Fi: connect timeout");
  return false;
}

bool wifiConnectWithFallback() {
  const WifiCred primary  { WIFI_SSID_1, WIFI_PASSWORD_1 };
  const WifiCred fallback { WIFI_SSID_2, WIFI_PASSWORD_2 };

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

#if WIFI_SCAN_BEFORE_CONNECT
  int n = WiFi.scanNetworks(false, true);
  bool primaryVisible = false;
  bool fallbackVisible = false;
  int32_t primaryRssi = -127;
  int32_t fallbackRssi = -127;

  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (primary.ssid && s == primary.ssid) {
      primaryVisible = true;
      primaryRssi = WiFi.RSSI(i);
    }
    if (fallback.ssid && s == fallback.ssid) {
      fallbackVisible = true;
      fallbackRssi = WiFi.RSSI(i);
    }
  }

  if (primaryVisible) {
    if (fallbackVisible) {
      bool preferPrimary = (primaryRssi >= fallbackRssi);
      if (lastConnectedSsid.length()) {
        if (lastConnectedSsid == primary.ssid) {
          preferPrimary = (primaryRssi + WIFI_RSSI_HYST_DB >= fallbackRssi);
        } else if (lastConnectedSsid == fallback.ssid) {
          preferPrimary = !(fallbackRssi + WIFI_RSSI_HYST_DB >= primaryRssi);
        }
      }
      if (preferPrimary) {
        if (tryConnect(primary, WIFI_CONNECT_TIMEOUT_MS)) return true;
        if (tryConnect(fallback, WIFI_CONNECT_TIMEOUT_MS)) return true;
      } else {
        if (tryConnect(fallback, WIFI_CONNECT_TIMEOUT_MS)) return true;
        if (tryConnect(primary, WIFI_CONNECT_TIMEOUT_MS)) return true;
      }
      return false;
    }

    if (tryConnect(primary, WIFI_CONNECT_TIMEOUT_MS)) return true;
    if (tryConnect(fallback, WIFI_CONNECT_TIMEOUT_MS)) return true;
    return false;
  }

  if (fallbackVisible) {
    if (tryConnect(fallback, WIFI_CONNECT_TIMEOUT_MS)) return true;
    if (tryConnect(primary, WIFI_CONNECT_TIMEOUT_MS)) return true;
    return false;
  }
#endif

  // If scan disabled or nothing found, just try priority order.
  if (tryConnect(primary, WIFI_CONNECT_TIMEOUT_MS)) return true;
  if (tryConnect(fallback, WIFI_CONNECT_TIMEOUT_MS)) return true;
  return false;
}

void wifiTick() {
  static uint32_t lastAttempt = 0;

  if (WiFi.status() == WL_CONNECTED) return;

  const uint32_t now = millis();
  if (now - lastAttempt < WIFI_RECONNECT_INTERVAL_MS) return;

  lastAttempt = now;
  wifiConnectWithFallback();
}
