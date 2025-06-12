// settings.cpp

#include "settings.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "ntp_sync.h"
#include "gsm_modem.h"
#include "mqtt_module.h"

static const char* SETTINGS_PATH = "/settings.json";
Settings settings;

void loadSettings() {
  if (!LittleFS.exists(SETTINGS_PATH)) {
    // Výchozí hodnoty…
    settings.ntpServer        = "pool.ntp.org";
    settings.ntpPort          = 123;
    settings.localPort        = 2390;
    settings.retryInterval    = 10000;
    settings.tzString         = "CET-1CEST,M3.5.0/2,M10.5.0/3";
    settings.baudRate         = 115200;
    settings.atctzu           = true;
    settings.atctr            = true;
    settings.atclip           = true;
    settings.smsPromptTimeout = 10000;
    settings.smsTimeout       = 15000;
    settings.cmdInterval      = 200;
    settings.maxRingCount     = 1;
    return;
  }

  File f = LittleFS.open(SETTINGS_PATH, "r");
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    settings.ntpServer        = doc["ntpServer"]        | settings.ntpServer;
    settings.ntpPort          = doc["ntpPort"]          | settings.ntpPort;
    settings.localPort        = doc["localPort"]        | settings.localPort;
    settings.retryInterval    = doc["retryInterval"]    | settings.retryInterval;
    settings.tzString         = doc["tzString"]         | settings.tzString;
    settings.baudRate         = doc["baudRate"]         | settings.baudRate;
    settings.atctzu           = doc["atctzu"]           | settings.atctzu;
    settings.atctr            = doc["atctr"]            | settings.atctr;
    settings.atclip           = doc["atclip"]           | settings.atclip;
    settings.smsPromptTimeout = doc["smsPromptTimeout"] | settings.smsPromptTimeout;
    settings.smsTimeout       = doc["smsTimeout"]       | settings.smsTimeout;
    settings.cmdInterval      = doc["cmdInterval"]      | settings.cmdInterval;
    settings.maxRingCount     = doc["maxRingCount"]     | settings.maxRingCount;
  }
  f.close();
}

/// Zapíše aktuální `settings` do `/settings.json`, vrací true při úspěchu
bool saveSettings() {
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) return false;

  StaticJsonDocument<512> doc;
  doc["ntpServer"]        = settings.ntpServer;
  doc["ntpPort"]          = settings.ntpPort;
  doc["localPort"]        = settings.localPort;
  doc["retryInterval"]    = settings.retryInterval;
  doc["tzString"]         = settings.tzString;
  doc["baudRate"]         = settings.baudRate;
  doc["atctzu"]           = settings.atctzu;
  doc["atctr"]            = settings.atctr;
  doc["atclip"]           = settings.atclip;
  doc["smsPromptTimeout"] = settings.smsPromptTimeout;
  doc["smsTimeout"]       = settings.smsTimeout;
  doc["cmdInterval"]      = settings.cmdInterval;
  doc["maxRingCount"]     = settings.maxRingCount;

  size_t written = serializeJson(doc, f);
  f.close();
  return written > 0;
}

void applySettings() {
  // 1) Timezone
  setenv("TZ", settings.tzString.c_str(), 1);
  tzset();

  // 2) NTP synchronizace
  ntpBegin();

  // 3) Sériová linka
  Serial.begin(settings.baudRate);

  // 4) GSM modem
  sendAtCommand("AT+CTZU="  + String(settings.atctzu ? 1 : 0), "OK");
  sendAtCommand("AT+CTZR="  + String(settings.atctr  ? 1 : 0), "OK");
  sendAtCommand("AT+CLIP=" + String(settings.atclip ? 1 : 0), "OK");

  // 5) Ring count
  saveRingSetting(settings.maxRingCount);

  // 6) MQTT reconnect
  restartMqttConnection();
}
