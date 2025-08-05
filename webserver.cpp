// webserver.cpp (plně opravená verze s HTTP Basic Auth a funkcionalitou call-log API GET)
#include "webserver.h"
#include "gsm_modem.h"
#include "settings.h"
#include "ota_update.h"
#include <SPI.h>
#include <Ethernet.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "ntp_sync.h"

#define W5500_RESET_PIN 5

static const int CS_PIN = 5;
static byte mac[] = {0xDE,0xAD,0xBE,0xEF,0xFE,0xED};
static EthernetServer httpServer(80);
const char* SETTINGS_FILE = "/settings.json";

// --- Admin účet ---
static String adminPassword = "admin";
static const String adminUser = "admin";
static const char* PASSWD_PATH = "/admin_pass.txt";

// Pomocná funkce pro převod stavu na řetězec
const char* smsStateToString(SmsState st) {
  switch (st) {
    case SMS_IDLE:           return "idle";
    case SMS_SET_TEXT_MODE:
    case SMS_SEND_HEADER:
    case SMS_WAIT_PROMPT:
    case SMS_SEND_BODY:
    case SMS_WAIT_OK:        return "sending";
    case SMS_DONE:           return "sent";
    case SMS_ERROR:          return "error";
  }
  return "unknown";
}

// === Funkce načítání nastavení ===
void handleGetSettings(EthernetClient &client) {
  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) {
    sendJsonResponse(client, 200, "{}"); // prázdné nastavení
    return;
  }
  
  String settingsJson = f.readString();
  f.close();
  sendJsonResponse(client, 200, settingsJson);
}

// === Funkce uložení nastavení ===
// webserver.cpp

// webserver.cpp (výřez z handleSaveSettings)

void handleSaveSettings(EthernetClient &client, const String &body) {
  StaticJsonDocument<1024> doc;
  auto err = deserializeJson(doc, body);
  if (err) {
    sendError(client, 400, "Invalid JSON");
    return;
  }
  // Validace povinných polí…
  if (!doc.containsKey("ntpServer") || !doc.containsKey("ntpPort")) {
    sendError(client, 400, "Missing required fields");
    return;
  }

  // Aktualizace struktury
  settings.ntpServer     = doc["ntpServer"].as<String>();
  settings.ntpPort       = doc["ntpPort"].as<uint16_t>();
  settings.localPort     = doc["localPort"].as<uint16_t>();
  // … ostatní pole …
  settings.maxRingCount  = doc["maxRingCount"].as<uint8_t>();

  // Uložení na FS
  if (!saveSettings()) {
    sendError(client, 500, "Failed to write settings");
    return;
  }

  // Okamžitá aplikace změn
  applySettings();

  sendJsonResponse(client, 200, "{\"success\":true}");
}


void loadAdminPassword() {
  if (LittleFS.exists(PASSWD_PATH)) {
    File f = LittleFS.open(PASSWD_PATH, "r");
    adminPassword = f.readStringUntil('\n');
    f.close();
    adminPassword.trim();
    if (adminPassword.length() == 0) adminPassword = "admin";
  }
}

void saveAdminPassword(const String& newPass) {
  File f = LittleFS.open(PASSWD_PATH, "w");
  f.print(newPass);
  f.close();
  adminPassword = newPass;
}

bool isBase64(unsigned char c) {
  return (isalnum(c) || (c == '+') || (c == '/'));
}

// Jednoduchý Base64 dekodér pro Basic Auth (ASCII only)
String base64decode(const String &input) {
  const char* base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";
  int in_len = input.length();
  int i = 0;
  int in_ = 0;
  int char_array_4[4], char_array_3[3];
  String ret;

  while (in_len-- && (input[in_] != '=') && isBase64(input[in_])) {
    char_array_4[i++] = input[in_]; in_++;
    if (i == 4) {
      for (i = 0; i < 4; i++)
        char_array_4[i] = strchr(base64_chars, char_array_4[i]) - base64_chars;
      char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
      char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
      char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
      for (i = 0; (i < 3); i++) ret += (char) char_array_3[i];
      i = 0;
    }
  }
  if (i) {
    for (int j = i; j < 4; j++) char_array_4[j] = 0;
    for (int j = 0; j < 4; j++)
      char_array_4[j] = strchr(base64_chars, char_array_4[j]) - base64_chars;
    char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
    char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
    char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
    for (int j = 0; (j < i - 1); j++) ret += (char) char_array_3[j];
  }
  return ret;
}

// Kontrola HTTP Basic Auth
bool checkAuth(EthernetClient &client) {
  String line;
  bool authorized = false;
  while (client.available()) {
    line = client.readStringUntil('\n');
    if (line.startsWith("Authorization: Basic ")) {
      String encoded = line.substring(21);
      encoded.trim();
      String decoded = base64decode(encoded);
      int sep = decoded.indexOf(':');
      if (sep > 0) {
        String user = decoded.substring(0, sep);
        String pass = decoded.substring(sep+1);
        if (user == adminUser && pass == adminPassword) {
          authorized = true;
        }
      }
      break;
    }
    if (line == "\r") break; // konec hlaviček
  }
  if (!authorized) {
    client.println("HTTP/1.1 401 Unauthorized");
    client.println("WWW-Authenticate: Basic realm=\"admin\"");
    client.println("Connection: close");
    client.println();
  }
  return authorized;
}

// Endpoint pro změnu hesla
void handleSetPassword(EthernetClient &client, const String &body) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  String newPass = doc["password"] | "";
  if (newPass.length() < 4) {
    client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"Password too short\"}");
    return;
  }
  saveAdminPassword(newPass);
  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"success\":true}");
}

void handleSaveContacts(EthernetClient &client, const String &body) {
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc.is<JsonArray>()) {
    client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"Invalid JSON array\"}");
    return;
  }

  File f = LittleFS.open("/contacts.json", "w");
  if (!f) {
    client.print("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"Failed to open file\"}");
    return;
  }

  serializeJson(doc, f);
  f.close();

  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"success\":true}");
}

void sendJsonResponse(EthernetClient &client, int statusCode, const String &json) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(" ");
  client.println((statusCode == 200) ? "OK" : "Bad Request");
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.println();
  client.print(json);
}

void sendError(EthernetClient &client, int statusCode, const String &message) {
  StaticJsonDocument<128> errorDoc;
  errorDoc["success"] = false;
  errorDoc["error"] = message;
  String out;
  serializeJson(errorDoc, out);
  sendJsonResponse(client, statusCode, out);
}

bool validateSmsRequest(const JsonDocument &doc, String &message, JsonArrayConst &recipients, String &error) {
  if (!doc.containsKey("message") || !doc["message"].is<const char*>()) {
    error = "Missing or invalid 'message'";
    return false;
  }
  message = doc["message"].as<const char*>();

  JsonVariantConst recVar = doc["recipients"];
  if (!recVar.is<JsonArrayConst>()) {
    error = "Missing or invalid 'recipients' array";
    return false;
  }
  recipients = recVar.as<JsonArrayConst>();

  if (recipients.size() == 0) {
    error = "Recipients array is empty";
    return false;
  }
  return true;
}

void recordSmsToHistory(const String& recipient, const String& message) {
  // 1) Načíst stávající pole
  StaticJsonDocument<4096> histDoc;
  JsonArray oldArr;
  if (LittleFS.exists("/sms_history.json")) {
    File fr = LittleFS.open("/sms_history.json", "r");
    if (deserializeJson(histDoc, fr) == DeserializationError::Ok)
      oldArr = histDoc.as<JsonArray>();
    fr.close();
  }
  // 2) Vytvořit nový dokument s prvním záznamem
  StaticJsonDocument<256> rec;
  rec["timestamp"] = time(nullptr);
  rec["recipient"] = recipient;
  rec["message"]   = message;

  StaticJsonDocument<4096> outDoc;
  JsonArray outArr = outDoc.to<JsonArray>();
  outArr.add(rec);

  // 3) Doplnit staré záznamy až do limitu
  uint16_t maxC = getSmsHistoryMaxCount();
  uint16_t count = 1;
  for (JsonVariant v : oldArr) {
    if (count++ >= maxC) break;
    outArr.add(v);
  }
  // 4) Zápis zpět na FS
  File fw = LittleFS.open("/sms_history.json", "w");
  serializeJson(outArr, fw);
  fw.close();
}

void handleSendSms(EthernetClient &client, const String &body) {
  Serial.println(F("[Webserver] Přijat požadavek na odeslání SMS"));
  Serial.print(F("[Webserver] Tělo požadavku: "));
  Serial.println(body);

  StaticJsonDocument<512> doc;
  DeserializationError deserErr = deserializeJson(doc, body);
  if (deserErr) {
    Serial.print(F("[Webserver] Chyba: Neplatný JSON: "));
    Serial.println(deserErr.c_str());
    sendError(client, 400, "Invalid JSON");
    return;
  }

  String messageText;
  JsonArrayConst recipients;
  String validationError;
  if (!validateSmsRequest(doc, messageText, recipients, validationError)) {
    Serial.print(F("[Webserver] Chyba validace: "));
    Serial.println(validationError);
    sendError(client, 400, validationError);
    return;
  }

  Serial.print(F("[Webserver] Počet příjemců: "));
  Serial.println(recipients.size());
  Serial.print(F("[Webserver] Zpráva: "));
  Serial.println(messageText);

  StaticJsonDocument<1024> result;
  JsonArray sent   = result.createNestedArray("sent");
  JsonArray failed = result.createNestedArray("failed");

  for (JsonVariantConst v : recipients) {
    if (!v.is<const char*>()) {
      Serial.println(F("[Webserver] Neplatný typ příjemce"));
      failed.add("invalid_type");
      continue;
    }
    String num = v.as<const char*>();
    num.trim();
    if (num.length() < 6) {
      Serial.print(F("[Webserver] Neplatné číslo: "));
      Serial.println(num);
      failed.add(num);
      continue;
    }

    Serial.print(F("[Webserver] Odesílání SMS na: "));
    Serial.println(num);
    bool ok = sendSmsNow(num, messageText);
    if (ok) {
      Serial.println(F("[Webserver] SMS úspěšně odeslána"));
      recordSmsToHistory(num, messageText);
      sent.add(num);
    } else {
      Serial.println(F("[Webserver] Chyba při odeslání SMS"));
      failed.add(num);
    }
  }

  result["success"]      = (failed.size() == 0);
  result["total"]        = recipients.size();
  result["sent_count"]   = sent.size();
  result["failed_count"] = failed.size();

  String response;
  serializeJson(result, response);
  Serial.print(F("[Webserver] Výsledek: "));
  Serial.println(response);

  sendJsonResponse(client, 200, response);
}

void handleScheduleSms(EthernetClient &client, const String &body) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  JsonArray numbers = doc["numbers"];
  String message = doc["message"] | "";
  String sendTime = doc["sendTime"] | "";

  if (!numbers || message.length() == 0 || sendTime.length() == 0) {
    client.print("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"Missing fields\"}");
    return;
  }

  // Uložení naplánované SMS do JSON souboru nebo do plánovací fronty
  File f = LittleFS.open("/scheduled_sms.json", "a");
  if (!f) {
    client.print("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n{\"success\":false,\"error\":\"Failed to store SMS\"}");
    return;
  }

  StaticJsonDocument<256> smsDoc;
  smsDoc["numbers"] = numbers;
  smsDoc["message"] = message;
  smsDoc["sendTime"] = sendTime;

  serializeJson(smsDoc, f);
  f.print("\n");
  f.close();

  client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"success\":true}");
}

void handleSendAtCommand(EthernetClient &client, const String &body) {
/*
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    sendError(client, 400, "Invalid JSON");
    return;
  }

  String cmd = doc["command"] | "";
  cmd.trim();
  if (cmd.isEmpty()) {
    sendError(client, 400, "Missing AT command");
    return;
  }

  // Odeslat do UART modemu (např. Serial2)
  SerialGSM.println(cmd);

  // Čekat na odpověď
  String response = "";
  unsigned long timeout = millis() + 1000;  // 1 s timeout
  while (millis() < timeout) {
    while (SerialGSM.available()) {
      char c = SerialGSM.read();
      response += c;
    }
  }

  // Odpověď jako čistý text
  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
  client.print(response);
*/
}

// Podávání statických souborů
static void serveFile(EthernetClient& client, const char* path, const char* contentType) {
  if (!LittleFS.exists(path)) {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Connection: close");
    client.println();
    return;
  }
  File f = LittleFS.open(path, "r");
  client.print("HTTP/1.1 200 OK\r\nContent-Type: ");
  client.print(contentType);
  client.print("\r\nConnection: close\r\n\r\n");
  uint8_t buf[64];
  while (size_t n = f.read(buf, sizeof(buf))) {
    client.write(buf, n);
  }
  f.close();
}

void resetW5500() {
  pinMode(W5500_RESET_PIN, OUTPUT);
  digitalWrite(W5500_RESET_PIN, LOW);
  delay(100);  // minimálně 50ms
  digitalWrite(W5500_RESET_PIN, HIGH);
  delay(200);  // počkej na inicializaci W5500
}

void networkInit() {
  resetW5500();
  SPI.begin(18, 19, 23); // Lze upravit podle HW
  Ethernet.init(CS_PIN);
  Serial.print("DHCP… ");
  if (Ethernet.begin(mac) == 0) {
    Serial.println("selhalo");
  } else {
    Serial.println("OK, IP=" + Ethernet.localIP().toString());
  }
  if (!LittleFS.begin(true)) {
    Serial.println("FS mount failed");
  }
  loadSettings();
  applySettings();
  /*
  // aplikuj timezone hned po načtení
  setenv("TZ", settings.tzString.c_str(), 1);
  tzset();
  // a první NTP synchronizaci
  */
  ntpBegin();
  loadAdminPassword();
  loadSmsHistoryMaxCount();
  httpServer.begin();
  otaInit();
  Serial.println("HTTP server běží");
}

void networkLoop() {
  EthernetClient client = httpServer.available();
  if (!client) return;

  // přečtení request line
  String request = client.readStringUntil('\r');
  client.read();  // skip '\n'

  bool isGet  = request.startsWith("GET ");
  bool isPost = request.startsWith("POST ");
  int sp1 = request.indexOf(' ') + 1;
  int sp2 = request.indexOf(' ', sp1);
  String path = request.substring(sp1, sp2);
  String method = isGet ? "GET" : (isPost ? "POST" : "");

  // OTA endpoint vyřešíme jako první
  if (otaHandle(client, method, path)) return;

  // --- Basic Auth for all /api/ endpoints ---
  if (path.startsWith("/api/")) {
    if (!checkAuth(client)) {
      delay(1);
      client.stop();
      return;
    }
  }

  // --- POST: set password ---
  if (isPost && path == "/api/set-password") {
    // skip headers
    while (client.available()) {
      String h = client.readStringUntil('\n');
      if (h == "\r") break;
    }
    String body = client.readStringUntil('\0');
    handleSetPassword(client, body);
    delay(1);
    client.stop();
    return;
  }

  // --- POST: AT příkaz z webu ---
  if (isPost && path == "/api/at/send") {
    int contentLength = 0;
    while (client.available()) {
      String headerLine = client.readStringUntil('\n');
      headerLine.trim();
      if (headerLine.length() == 0) break;
      if (headerLine.startsWith("Content-Length:")) {
        contentLength = headerLine.substring(15).toInt();
      }
    }
    String body;
    while ((int)body.length() < contentLength) {
      if (client.available()) {
        body += (char)client.read();
      }
    }
    handleSendAtCommand(client, body);
    delay(1);
    client.stop();
    return;
  }


  // --- POST: uložení nastavení ---
if (isPost && path == "/api/settings") {
  // skip headers
  while (client.available()) {
    String h = client.readStringUntil('\n');
    if (h == "\r") break;
  }
  String body = client.readStringUntil('\0');
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
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
    // Uložení na FS a okamžitá aplikace všech nastavení
    if (!saveSettings()) {
      sendError(client, 500, "Failed to write settings");
      return;
    }
    applySettings();   // ← provede TZ, NTP, Serial, modem, ring-count i MQTT
    sendJsonResponse(client, 200, "{\"success\":true}");
  } else {
    sendError(client, 400, "invalid JSON");
  }
  delay(1); client.stop();
  return;
}

  // --- POST: save contacts ---
  if (isPost && path == "/api/save-contacts") {
    int contentLength = 0;
    while (client.available()) {
      String headerLine = client.readStringUntil('\n');
      headerLine.trim();
      if (headerLine.length() == 0) break;
      if (headerLine.startsWith("Content-Length:")) {
        contentLength = headerLine.substring(15).toInt();
      }
    }
    if (contentLength <= 0) {
      client.print("HTTP/1.1 411 Length Required\r\nContent-Type: application/json\r\n\r\n{\"error\":\"Content-Length required\"}");
      delay(1);
      client.stop();
      return;
    }
    String body;
    while (body.length() < contentLength) {
      if (client.available()) {
        body += (char)client.read();
      }
    }
    handleSaveContacts(client, body);
    delay(1);
    client.stop();
    return;
  }
  // --- POST: enqueue SMS tasks ---
if (isPost && path == "/api/send-sms") {
  // 1) Načti Content-Length
  int contentLength = 0;
  while (client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    }
  }

  // 2) Čti body do proměnné body
  String body;
  if (contentLength > 0) {
    while ((int)body.length() < contentLength) {
      if (client.available()) {
        body += (char)client.read();
      }
    }
  } else {
    unsigned long start = millis();
    while (millis() - start < 2000) {
      while (client.available()) {
        body += (char)client.read();
        start = millis();
      }
    }
  }

  // 3) Parsuj JSON
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, body);
  if (err) {
    sendJsonResponse(client, 400, "{\"error\":\"invalid JSON\"}");
    delay(1); client.stop();
    return;
  }

  // 4) Vyextrahuj pole recipients a zprávu
  JsonArray recs = doc["recipients"].as<JsonArray>();
  String msg   = doc["message"].as<String>();

  // 5) Vytvoř frontu úloh, jedna úloha na každý recipient
  int lastId = -1;
  for (auto v : recs) {
    String num = v.as<String>();
    lastId = enqueueSms(num, msg);  // vrátí unikátní taskId
    recordSmsToHistory(num, msg);
  }

  // 6) Odpověď s ID poslední úlohy
  StaticJsonDocument<128> res;
  res["status"] = "queued";
  res["id"]     = lastId;
  String out; serializeJson(res, out);
  sendJsonResponse(client, 200, out);

  delay(1);
  client.stop();
  return;
  }

  // --- GET requests ---
  if (isGet) {
    // Static files
    if (path == "/") {
      serveFile(client, "/index.html", "text/html");
      delay(1);
      client.stop();
      return;
    }
    else if (path.startsWith("/css/")) {
      serveFile(client, path.c_str(), "text/css");
      delay(1);
      client.stop();
      return;
    }
    else if (path.startsWith("/js/")) {
      serveFile(client, path.c_str(), "application/javascript");
      delay(1);
      client.stop();
      return;
    }
    else if (path.endsWith(".json")) {
      serveFile(client, path.c_str(), "application/json");
      delay(1);
      client.stop();
      return;
    }
    else if (path == "/api/settings") {
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
      String out; serializeJson(doc, out);
      sendJsonResponse(client, 200, out);
      delay(1); client.stop();
      return;
    }
      // --- Modem status API ---
    else if (path == "/api/modem-status") {
      uint8_t sig = readSignalQuality();
      String op    = readOperatorName();
      bool mqttOk  = mqttClient.connected();  // nebo jak píšete v kódu
      StaticJsonDocument<128> doc;
      doc["signal"]        = sig;
      doc["operator"]      = op;
      doc["mqttConnected"] = mqttOk;
      String out;
      serializeJson(doc, out);
      sendJsonResponse(client, 200, out);
      delay(1); client.stop();
      return;
    }
    // GET /api/sms-status
  else if (isGet && path == "/api/sms-status") {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.createNestedArray("queue");
    size_t n = getSmsQueueSize();
    for (size_t i = 0; i < n; ++i) {
      SmsTask t = getSmsQueueTask(i);
      SmsState s = getSmsTaskState(i);
      JsonObject o = arr.createNestedObject();
      o["id"]         = (int)i;
      o["recipients"] = t.recipients;
      o["message"]    = t.message;
      o["state"]      = smsStateToString(s);
    }
    String out; serializeJson(doc, out);
    sendJsonResponse(client, 200, out);
    delay(1); client.stop();
    return;
    }
    // --- GET: načtení nastavení ---
  if (isGet && path == "/api/settings") {
    handleGetSettings(client);
    delay(1);
    client.stop();
    return;
  } 
    // MQTT config
    else if (path == "/api/mqtt-config") {
      handleGetMqttConfig(client);
      delay(1);
      client.stop();
      return;
    }
    // Call log history
    else if (path == "/api/call-log") {
      client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n");
      if (LittleFS.exists("/call_log.json")) {
        File f = LittleFS.open("/call_log.json", "r");
        while (f.available()) client.write(f.read());
        f.close();
      } else {
        client.print("[]");
      }
      delay(1);
      client.stop();
      return;
    }
    // SMS history
    else if (path == "/api/sms-history") {
      sendJsonResponse(client, 200,
        String("{\"history\":") +
        (LittleFS.exists("/sms_history.json")
          ? []() {
              File f = LittleFS.open("/sms_history.json", "r");
              String s; while (f.available()) s += char(f.read());
              f.close();
              return s;
            }()
          : "[]")
        + "}"
      );
      delay(1);
      client.stop();
      return;
    }
    // Config GET
    else if (path == "/api/config") {
      StaticJsonDocument<128> c;
      c["smsHistoryMaxCount"] = getSmsHistoryMaxCount();
      String out;
      serializeJson(c, out);
      sendJsonResponse(client, 200, out);
      delay(1);
      client.stop();
      return;
    }
    // Undefined GET
    else {
      client.println("HTTP/1.1 404 Not Found");
      client.println("Connection: close");
      client.println();
      delay(1);
      client.stop();
      return;
    }
  }

  // --- Any other POST (fallback) ---
  if (isPost) {
    // skip remaining headers
    while (client.available()) {
      String h = client.readStringUntil('\n');
      if (h == "\r") break;
    }
    String body = client.readStringUntil('\0');

    if (path == "/api/contacts") {
      handleSaveContacts(client, body);
    }
    else if (path == "/api/mqtt-config") {
      handlePostMqttConfig(client, body);
    }
    else if (path == "/api/mqtt-test") {
      handleMqttTest(client, body);
    }
    else {
      client.println("HTTP/1.1 404 Not Found");
      client.println("Connection: close");
    }
    delay(1);
    client.stop();
    return;
  }

  // --- If we reach here, no known method matched ---
  client.println("HTTP/1.1 400 Bad Request");
  client.println("Connection: close");
  client.println();
  delay(1);
  client.stop();
}

