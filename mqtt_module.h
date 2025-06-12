// mqtt_module.h (optimalizovaná verze)
#pragma once

#include <PubSubClient.h>
#include <Arduino.h>
#include <Ethernet.h>
#include "gsm_modem.h"        // modemScheduleSMS()

// ======= Konfigurační struktura =======
struct MqttConfig {
  String  clientId, username, password, broker;
  uint16_t port = 1883;
  uint16_t keepalive = 60;
  bool    cleanSession = true;
  String  statusTopic, smsTopic, callerTopic, pubTopic;
};

extern MqttConfig   cfg;
extern PubSubClient mqttClient;

// ======= MQTT klient, inicializace, běh =======
void mqttModuleInit();
void mqttModuleLoop();

// ======= Ihned restartovat MQTT podle nové konfigurace =======
bool restartMqttConnection();

// ======= Publikace Caller ID na MQTT =======
void mqttPublishCaller(const String &caller);

// ======= HTTP API pro konfiguraci =======
void handleGetMqttConfig(EthernetClient &client);
void handlePostMqttConfig(EthernetClient &client, const String &body);
void handleMqttTest(EthernetClient &client, const String &body);
