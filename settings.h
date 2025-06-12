// settings.h

#pragma once

#include <Arduino.h>

/// All of your configuration parameters
struct Settings {
  String   ntpServer;
  uint16_t ntpPort;
  uint16_t localPort;
  uint32_t retryInterval;
  String   tzString;
  uint32_t baudRate;
  bool     atctzu;
  bool     atctr;
  bool     atclip;
  uint32_t smsPromptTimeout;
  uint32_t smsTimeout;
  uint32_t cmdInterval;
  uint8_t  maxRingCount;
};

extern Settings settings;

/// Load settings from LittleFS (or fill in defaults)
void loadSettings();

/// Save settings back to LittleFS; returns true on success
bool saveSettings();

/// Apply current settings immediately (TZ, NTP, serial, modem, MQTT, ring count…)
void applySettings();
