// mqtt_module.cpp (optimalizovaná verze)
#include "mqtt_module.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include "gsm_modem.h"

// ─── Forward declarations ────────────────────────────────────
// so that restartMqttConnection() can refer to these below
static void mqttCallback(char* topic, byte* payload, unsigned int length);
static const char* stateToString(int8_t state);

// ====== Konfigurace a proměnné ======
MqttConfig cfg;
WiFiClient netClient;
static EthernetClient ethClient;
PubSubClient mqttClient(ethClient);

constexpr const char* CONFIG_PATH = "/mqtt_config.json";
constexpr unsigned long RECONNECT_INTERVAL = 5000;

// ====== Pomocné makro pro debug výpisy ======
#ifndef MQTT_DEBUG
#define MQTT_DEBUG 1
#endif
#if MQTT_DEBUG
  #define MQTT_DBG(x) do { Serial.print(F("[MQTT] ")); Serial.println(x); } while(0)
#else
  #define MQTT_DBG(x)
#endif

// ====== Pomocné funkce pro JSON a konfiguraci ======
static bool loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return false;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  cfg.clientId     = doc["clientId"].as<String>();
  cfg.username     = doc["username"].as<String>();
  cfg.password     = doc["password"].as<String>();
  cfg.broker       = doc["broker"].as<String>();
  cfg.port         = doc["port"].as<uint16_t>();
  cfg.keepalive    = doc["keepalive"].as<uint16_t>();
  cfg.cleanSession = doc["cleanSession"].as<bool>();
  cfg.statusTopic  = doc["statusTopic"].as<String>();
  cfg.smsTopic     = doc["smsTopic"].as<String>();
  cfg.callerTopic  = doc["callerTopic"].as<String>();
  cfg.pubTopic     = doc["pubTopic"].as<String>();
  return true;
}

static bool saveConfig() {
  StaticJsonDocument<512> doc;
  doc["clientId"]     = cfg.clientId;
  doc["username"]     = cfg.username;
  doc["password"]     = cfg.password;
  doc["broker"]       = cfg.broker;
  doc["port"]         = cfg.port;
  doc["keepalive"]    = cfg.keepalive;
  doc["cleanSession"] = cfg.cleanSession;
  doc["statusTopic"]  = cfg.statusTopic;
  doc["smsTopic"]     = cfg.smsTopic;
  doc["callerTopic"]  = cfg.callerTopic;
  doc["pubTopic"]     = cfg.pubTopic;
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

// ======= Restart MQTT spojení dle cfg (použij v handlePostMqttConfig) =======
bool restartMqttConnection() {
  // 1) Ujisti se, že máme config na FS
  if (!loadConfig() || !cfg.broker.length()) {
    MQTT_DBG("⚠️ restartMQTT: žádný konfig");
    return false;
  }
  // 2) Pokud už jsme připojení, odpoj
  if (mqttClient.connected()) {
    mqttClient.disconnect();
    MQTT_DBG("⏹️ MQTT disconnected (restart)");
  }
  // 3) Nastav server a keepalive/zpětný callback
  mqttClient.setServer(cfg.broker.c_str(), cfg.port);
  mqttClient.setKeepAlive(cfg.keepalive);
  mqttClient.setCallback(mqttCallback);

  // 4) Pokus se připojit znovu
  MQTT_DBG(String("⏳ MQTT reconnect to ") + cfg.broker + ":" + cfg.port);
  bool ok = cfg.username.length()
    ? mqttClient.connect(
        cfg.clientId.c_str(),
        cfg.username.c_str(),
        cfg.password.c_str(),
        nullptr, 0, false, nullptr,
        cfg.cleanSession
      )
    : mqttClient.connect(cfg.clientId.c_str());
  if (!ok) {
    int8_t st = mqttClient.state();
    MQTT_DBG(String("❌ restartMQTT failed: ") + st + " (" + stateToString(st) + ")");
    return false;
  }
  MQTT_DBG("✅ restartMQTT ok");

  // 5) Přihlásit se na topicy
  if (cfg.statusTopic.length())  mqttClient.subscribe(cfg.statusTopic.c_str());
  if (cfg.smsTopic.length())     mqttClient.subscribe(cfg.smsTopic.c_str());
  if (cfg.callerTopic.length())  mqttClient.subscribe(cfg.callerTopic.c_str());
  return true;
}

// ====== Stavové řetězce pro PubSubClient ======
static const char* stateToString(int8_t s) {
  switch(s) {
    case MQTT_CONNECTION_TIMEOUT:      return "CONNECTION_TIMEOUT";
    case MQTT_CONNECTION_LOST:         return "CONNECTION_LOST";
    case MQTT_CONNECT_FAILED:          return "CONNECT_FAILED";
    case MQTT_DISCONNECTED:            return "DISCONNECTED";
    case MQTT_CONNECTED:               return "CONNECTED";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "BAD_PROTOCOL";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "BAD_CLIENT_ID";
    case MQTT_CONNECT_UNAVAILABLE:     return "UNAVAILABLE";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "BAD_CREDENTIALS";
    case MQTT_CONNECT_UNAUTHORIZED:    return "UNAUTHORIZED";
    default:                           return "UNKNOWN";
  }
}

// ====== MQTT Callback ======
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg((char*)payload, length);
  MQTT_DBG(String("📥 MQTT zpráva na [") + topic + "]: " + msg);
  String tp(topic);

  // 1) Odeslání SMS přes frontu
  if (tp == cfg.smsTopic) {
    StaticJsonDocument<256> j;
    if (deserializeJson(j, msg) == DeserializationError::Ok) {
      String rec = j["recipients"].as<String>();
      String txt = j["message"].   as<String>();
      bool ok = modemScheduleSMS(rec, txt, "");
      // ACK
      if (cfg.pubTopic.length()) {
        StaticJsonDocument<128> ack;
        ack["status"]     = ok ? "queued" : "error";
        ack["recipients"] = rec;
        ack["message"]    = txt;
        char buf[128];
        size_t n = serializeJson(ack, buf);
        mqttClient.publish(cfg.pubTopic.c_str(), buf, n);
      }
    } else {
      MQTT_DBG("❌ JSON CHYBA SMS");
    }
  }
  // 2) Stav zařízení
  else if (tp == cfg.statusTopic) {
    StaticJsonDocument<128> st;
    st["uptime"]   = millis() / 1000;
    st["freeHeap"] = ESP.getFreeHeap();
    char buf[128];
    size_t n = serializeJson(st, buf);
    if (cfg.pubTopic.length())
      mqttClient.publish(cfg.pubTopic.c_str(), buf, n);
  }
  // 3) Caller ID (pro případné rozšíření)
  else if (tp == cfg.callerTopic) {
    // Volitelně: zpracovat zprávu (např. příchozí volání)
    // Zatím není implementováno
  }
}

// ====== Inicializace MQTT modulu ======
void mqttModuleInit() {
  if (!LittleFS.begin()) Serial.println("⚠️ LittleFS mount failed");
  if (loadConfig()) {
    MQTT_DBG(String("⚡ MQTT config: ") + cfg.broker + ':' + cfg.port + "  clientId=" + cfg.clientId);
    mqttClient.setServer(cfg.broker.c_str(), cfg.port);
    mqttClient.setKeepAlive(cfg.keepalive);
    mqttClient.setCallback(mqttCallback);
  } else {
    MQTT_DBG("⚠️ MQTT config nenalezen");
  }
}

// ====== Smyčka + reconnect + subscribe ======
void mqttModuleLoop() {
  if (!cfg.broker.length()) return;
  static unsigned long lastReconnect = 0;
  unsigned long now = millis();
  if (!mqttClient.connected()) {
    if (now - lastReconnect < RECONNECT_INTERVAL) return;
    lastReconnect = now;
    MQTT_DBG(String("⏳ MQTT reconnect to ") + cfg.broker + ':' + cfg.port + "…");
    bool ok = cfg.username.length()
      ? mqttClient.connect(cfg.clientId.c_str(), cfg.username.c_str(), cfg.password.c_str())
      : mqttClient.connect(cfg.clientId.c_str());
    if (ok) {
      MQTT_DBG("✅ MQTT připojeno");
      if (cfg.statusTopic.length()) mqttClient.subscribe(cfg.statusTopic.c_str());
      if (cfg.smsTopic.length())    mqttClient.subscribe(cfg.smsTopic.c_str());
      if (cfg.callerTopic.length()) mqttClient.subscribe(cfg.callerTopic.c_str());
    } else {
      int8_t st = mqttClient.state();
      MQTT_DBG(String("❌ MQTT fail: ") + st + " (" + stateToString(st) + ")");
    }
    return;
  }
  mqttClient.loop();
}

// ====== Publikace Caller ID na MQTT ======
void mqttPublishCaller(const String &caller) {
  if (cfg.callerTopic.length() > 0 && mqttClient.connected()) {
    MQTT_DBG(String("⬆️ MQTT publish [") + cfg.callerTopic + "]: " + caller);
    mqttClient.publish(cfg.callerTopic.c_str(), caller.c_str());
  } else {
    MQTT_DBG("⚠️ mqttPublishCaller skipped: no topic or MQTT disconnected");
  }
}

// ====== HTTP API handlery ======
void handleGetMqttConfig(EthernetClient &client) {
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n");
  if (LittleFS.exists(CONFIG_PATH)) {
    File f = LittleFS.open(CONFIG_PATH, "r");
    while (f.available()) client.write(f.read());
    f.close();
  } else {
    client.print("{}");
  }
}

void handlePostMqttConfig(EthernetClient &client, const String &body) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    cfg.clientId     = doc["clientId"].as<String>();
    cfg.username     = doc["username"].as<String>();
    cfg.password     = doc["password"].as<String>();
    cfg.broker       = doc["broker"].as<String>();
    cfg.port         = doc["port"].as<uint16_t>();
    cfg.keepalive    = doc["keepalive"].as<uint16_t>();
    cfg.cleanSession = doc["cleanSession"].as<bool>();
    cfg.statusTopic  = doc["statusTopic"].as<String>();
    cfg.smsTopic     = doc["smsTopic"].as<String>();
    cfg.callerTopic  = doc["callerTopic"].as<String>();
    cfg.pubTopic     = doc["pubTopic"].as<String>();
    saveConfig();
    // ihned restartuj MQTT podle nové konfigurace
    if (!restartMqttConnection()) {
      MQTT_DBG("⚠️ MQTT reconnect after config update failed");
    }
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"success\":true}");
    } else {
    client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"success\":false}");
  }
}

void handleMqttTest(EthernetClient &client, const String &body) {
  StaticJsonDocument<256> doc;
  bool ok = false; String err;
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    const char* cid = doc["clientId"];
    const char* usr = doc["username"];
    const char* pwd = doc["password"];
    const char* brk = doc["broker"];
    uint16_t prt = doc["port"].as<uint16_t>();
    uint16_t ka  = doc["keepalive"].as<uint16_t>();
    mqttClient.setServer(brk, prt);
    mqttClient.setKeepAlive(ka);
    ok = strlen(usr)
       ? mqttClient.connect(cid, usr, pwd)
       : mqttClient.connect(cid);
    if (!ok) err = stateToString(mqttClient.state());
  } else {
    err = "invalid JSON";
  }
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n");
  client.printf("{\"success\":%s", ok ? "true" : "false");
  if (!ok) client.printf(",\"error\":\"%s\"", err.c_str());
  client.print("}");
}
