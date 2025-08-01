// webserver.h (upravená verze)
// původní obsah viz :contentReference[oaicite:1]{index=1}

#pragma once

#include <Arduino.h>
#include <Ethernet.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "gsm_modem.h"       // modemScheduleSMS()
#include "mqtt_module.h"     // handleGetMqttConfig(), handlePostMqttConfig(), handleMqttTest()

size_t getSmsQueueSize();
SmsTask getSmsQueueTask(size_t idx);
SmsState getSmsTaskState(size_t idx);

// ======= Inicializace a obsluha HTTP serveru =======
void networkInit();  // Inicializace sítě, FS, spuštění serveru
void networkLoop();  // Běh webserveru (volat v loop)
void handleSaveSettings(EthernetClient &client, const String &body);
void handleGetSettings(EthernetClient &client);

// ======= Autorizace a správa hesla admina =======
void loadAdminPassword();
void saveAdminPassword(const String& newPass);
bool checkAuth(EthernetClient &client);
void handleSetPassword(EthernetClient &client, const String &body);
void handleSendSms(EthernetClient &client, const String &body);
void handleSendAtCommand(EthernetClient &client, const String &body);
void resetW5500();

// Odeslání JSON odpovědi
void sendJsonResponse(EthernetClient &client, int statusCode, const String &json);

// Odeslání chybové odpovědi ve formátu JSON
void sendError(EthernetClient &client, int statusCode, const String &message);

// Validace požadavku na SMS
bool validateSmsRequest(const JsonDocument &doc, String &message, JsonArrayConst &recipients, String &error);

// Funkce pro okamžité odeslání SMS (deklarována jinde – v gsm_modem.h)
bool sendSmsNow(const String &number, const String &message);

// ======= **Nová deklarace pro zápis do SMS historie** =======
void recordSmsToHistory(const String& recipient, const String& message);
