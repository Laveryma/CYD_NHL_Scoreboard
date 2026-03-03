#pragma once
#include <Arduino.h>

// Call once in setup() (it will attempt primary or fallback).
bool wifiConnectWithFallback();

// Call frequently in loop() to keep Wi-Fi up (retries periodically if disconnected).
void wifiTick();