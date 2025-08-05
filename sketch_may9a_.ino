/*
 * sketch_may9a.ino – hlavní soubor projektu GSM SMS Brána
 * Optimalizovaná verze s podporou HTTP Basic Auth (změna hesla),
 * MQTT, GSM/SMS a web server API.
 *
 * Nezapomeň přidat závislosti: Ethernet, LittleFS, ArduinoJson, PubSubClient, Base64 (Arduino)
 */

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <LittleFS.h>
#include "gsm_modem.h"
#include "mqtt_module.h"
#include "webserver.h"
#include "ntp_sync.h"
#include "settings.h"

void setup() {
  Serial.begin(9600);
  delay(2000);
  LittleFS.begin(true);
  loadSettings();    // načteme settings.json
  ntpBegin();        // spustíme první NTP požadavek
  networkInit();     // inicializujeme webserver, modemy...
  modemInit();
  setupDTR();
  mqttModuleInit();
}


void loop() {
  ntpIsSynced();
  networkLoop();          // Webserver (HTTP API a statické soubory)
  mqttModuleLoop();       // MQTT klient (příjem/publikace zpráv, reconnecty)
  handleModemURC();
  processSmsQueue();      // Zpracování příchozích SMS a fronty pro GSM
}
