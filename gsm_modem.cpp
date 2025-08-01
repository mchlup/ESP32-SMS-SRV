// gsm_modem.cpp (optimalizovaná verze)
#include "gsm_modem.h"
#include "mqtt_module.h"
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include "settings.h"

// ====== Konfigurace a konstanty ======
HardwareSerial SerialGSM(2);
//extern HardwareSerial SerialGSM(2);
static String urcBuffer;
static unsigned long ringStartTimestamp = 0;
static uint16_t ringCount = 0;
static String lastCaller = "";
static const char* CALL_LOG_PATH = "/call_log.json";
static const char* SMS_HISTORY_CONF = "/sms_history.conf";
static uint16_t smsHistoryMaxCount = 50;  // výchozí hodnota
// --- Uživatelsky nastavitelný počet RING před zavěšením (default 1) ---
static uint8_t maxRingCount = 1;
static const char* RINGS_PATH = "/rings.conf";

constexpr uint8_t MAX_TASKS = 8;
constexpr unsigned long CMD_INTERVAL = 200;      // Pauza mezi AT příkazy [ms]
constexpr unsigned long SMS_TIMEOUT  = 15000;    // Timeout na +CMGS [ms]
constexpr unsigned long SMS_PROMPT_TIMEOUT    = 10000;     // Timeout na prompt '>' [ms]

static unsigned long lastSmsQueueStatusLog = 0;
const unsigned long smsQueueStatusLogInterval = 15000; // ms

// ====== Datové struktury fronty ======
struct SmsTaskQueue {
  SmsTask buffer[MAX_TASKS];
  uint8_t head = 0, tail = 0;

  bool isEmpty() const { return head == tail; }
  bool isFull()  const { return ((tail + 1) % MAX_TASKS) == head; }

  bool enqueue(const SmsTask& task) {
    if (isFull()) return false;
    buffer[tail] = task;
    tail = (tail + 1) % MAX_TASKS;
    return true;
  }

  SmsTask dequeue() {
    SmsTask t = buffer[head];
    head = (head + 1) % MAX_TASKS;
    return t;
  }

  int size() const {
    if (tail >= head)
      return tail - head;
    else
      return MAX_TASKS - head + tail;
  }
};

static SmsTaskQueue smsQueue;

// ====== Stavový automat pro SMS ======
static SmsState      smsState       = SMS_IDLE;
static SmsTask       currentTask;
static unsigned long stateTimestamp = 0;
static String        responseBuf;

// ====== Pomocné makra pro debug výpis ======
#ifndef GSM_DEBUG
  #define GSM_DEBUG 1
#endif

#if GSM_DEBUG
  #define GSM_DBG_FMT(fmt, ...) do { \
    Serial.print(F("[GSM] ")); \
    Serial.printf((fmt), ##__VA_ARGS__); \
    Serial.println(); \
  } while(0)

  #define GSM_DBG(x) do { \
    Serial.print(F("[GSM] ")); \
    Serial.println(x); \
  } while(0)
#else
  #define GSM_DBG_FMT(fmt, ...) do {} while(0)
  #define GSM_DBG(x) do {} while(0)
#endif

// ====== Pomocné funkce ======
uint16_t getSmsHistoryMaxCount() {
  return smsHistoryMaxCount;
}
void setSmsHistoryMaxCount(uint16_t v) {
  File f = LittleFS.open(SMS_HISTORY_CONF, "w");
  if (f) {
    f.print(v);
    f.close();
    smsHistoryMaxCount = v;
  }
}
void loadSmsHistoryMaxCount() {
  if (LittleFS.exists(SMS_HISTORY_CONF)) {
    File f = LittleFS.open(SMS_HISTORY_CONF, "r");
    smsHistoryMaxCount = f.parseInt() ?: smsHistoryMaxCount;
    f.close();
  }
}

static void flushSerialGSM() {
  while (SerialGSM.available()) SerialGSM.read();
}

// Pošle řádek s CR+LF a zároveň to zobrazí v logu
static void sendAndPrint(const char* cmd, unsigned long timeout = 1000) {
  flushSerialGSM(); 
  GSM_DBG(String(F("> ")) + cmd);
  SerialGSM.println(cmd);
  
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (SerialGSM.available()) {
      String line = SerialGSM.readStringUntil('\n');
      line.trim();
      GSM_DBG(String(F("  < ")) + line);
      if (line == "OK" || line == "ERROR") {
        break;
      }
    }
  }
}

// Čeká na řádek obsahující buď "OK", "ERROR" nebo "+CMGS:"
// Vrací true, pokud přišlo "+CMGS:", false pokud "ERROR" nebo timeout
static bool waitForOkOrErrorOrCmgs(unsigned long timeout) {
  unsigned long t0 = millis();
  String line;
  while (millis() - t0 < timeout) {
    if (SerialGSM.available()) {
      line = SerialGSM.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      GSM_DBG(String(F("  < ")) + line);
      if (line.indexOf("+CMGS:") != -1) {
        return true;
      }
      if (line == "OK") {
        // modul v některých případech vyšle OK až po +CMGS, ale pokud
        // ještě nevidíme +CMGS, pokračujeme číst dokud nevyprší.
        continue;
      }
      if (line == "ERROR") {
        return false;
      }
    }
  }
  return false;
}

// Čeká na znak '>' v odpovědích modemu, vrací true pokud přišel prompt včas
static bool waitForPrompt(unsigned long timeout) {
  unsigned long t0 = millis();
  while (millis() - t0 < timeout) {
    if (SerialGSM.available()) {
      char c = SerialGSM.read();
      responseBuf += c;
      // GSM_DBG(String(F("  buf: ")) + responseBuf);
      if (responseBuf.indexOf('>') != -1) {
        return true;
      }
    }
  }
  return false;
}

void handleModemURC() {
  while (SerialGSM.available()) {
    char c = SerialGSM.read();
    urcBuffer += c;
    // každý CR/LF končí jedna URC řádka
    if (c == '\n') {
      String line = urcBuffer;
      line.trim();          // odstraní \r i mezery
      urcBuffer = "";
      if (line.length() == 0) return;

      GSM_DBG(String(F("[URC] ")) + line);
      processCallerIDLine(line);
      processSmsResponseLine(line);
    }
  }
}
/*
void startNtpSync() {
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
    //configTime(3600, 3600, "216.239.35.0");
    //configTime(3600, 3600, "172.24.13.1");
    ntpSyncInProgress = true;
    ntpSyncStart = millis();
    ntpSyncTries = 0;
    ntpSyncReported = false;
    Serial.print(F("Spouštím asynchronní synchronizaci času (NTP)... "));
}

void processNtpSync() {
    if (!ntpSyncInProgress) return;
    time_t now = time(nullptr);
    if (!ntpSyncReported && now > 1672531200) {
        struct tm *t = localtime(&now);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
        Serial.print(F("Synchronizováno, aktuální čas: "));
        Serial.println(buf);
        ntpSyncReported = true;
        ntpSyncInProgress = false;
        return;
    }
    if (millis() - ntpSyncStart > 20000) { // timeout 20s
        if (!ntpSyncReported) {
            Serial.println(F("Synchronizace času SELHALA (timeout)!"));
            ntpSyncReported = true;
        }
        ntpSyncInProgress = false;
    }
    // Jinak čekej dále – funkce je neblokující
}
*/
void printModemSettings() {
  Serial.println(F("=== Modem settings ==="));
  const char* cmds[] = {
    "ATI", "AT+CSQ", "AT+CREG?", "AT+CGATT?", "AT+COPS?", "AT+CPIN?", "AT+CCID"
  };
  for (const char* cmd : cmds) {
    sendAndPrint(cmd);
    delay(1500);      // pauza 1,5 s mezi příkazy
    flushSerialGSM(); // očistíme buffer
  }
  Serial.println(F("=== End of settings ==="));
}

// ====== Časová logika (NTP) ======
String getCurrentTimeString() {
  time_t now = time(nullptr);
  struct tm tminfo;
  localtime_r(&now, &tminfo);
  char buf[25];
  // ISO-8601: YYYY-MM-DDTHH:MM:SS
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tminfo);
  return String(buf);
}

// ---- Nové helpery pro modem-status ----
String queryModem(const char* cmd, unsigned long timeout=1000) {
  // odešle příkaz, počká na OK a vrátí vše mezi (včetně +XXX: …)
  responseBuf = "";
  SerialGSM.println(cmd);
  unsigned long t0 = millis();
  while (millis() - t0 < timeout) {
    if (SerialGSM.available()) {
      String line = SerialGSM.readStringUntil('\n');
      line.trim();
      if (line.length()==0) continue;
      responseBuf += line + "\n";
      if (line == "OK" || line == "ERROR") break;
    }
  }
  return responseBuf;
}

uint8_t readSignalQuality() {
  String out = queryModem("AT+CSQ");
  int idx = out.indexOf("+CSQ:");
  if (idx >= 0) {
    int comma = out.indexOf(',', idx);
    return out.substring(idx + 5, comma).toInt();
  }
  return 0;
}

String readOperatorName() {
  String out = queryModem("AT+COPS?");
  int idx = out.indexOf("+COPS:");
  if (idx >= 0) {
    int q1 = out.indexOf('"', idx);
    int q2 = out.indexOf('"', q1 + 1);
    return out.substring(q1 + 1, q2);
  }
  return String("--");
}

// ====== Logování volání ======
void logCallToFile(const String& caller) {
  StaticJsonDocument<8192> doc;
  JsonArray arr;
  File f = LittleFS.open(CALL_LOG_PATH, "r");
  if (f) {
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      doc.clear();
      arr = doc.to<JsonArray>();
    } else {
      if (!doc.is<JsonArray>()) {
        doc.clear();
        arr = doc.to<JsonArray>();
      } else {
        arr = doc.as<JsonArray>();
      }
    }
  } else {
    arr = doc.to<JsonArray>();
  }

  // Přidej nový záznam
  JsonObject entry = arr.createNestedObject();
  entry["datetime"] = getCurrentTimeString();
  entry["number"] = caller;

  // Udržuj jen posledních 10 záznamů
  while (arr.size() > 10) arr.remove(0);

  // Ulož zpět
  f = LittleFS.open(CALL_LOG_PATH, "w");
  serializeJson(doc, f);
  f.close();
}

// ====== RING nastavení ======
void loadRingSetting() {
  if (LittleFS.exists(RINGS_PATH)) {
    File f = LittleFS.open(RINGS_PATH, "r");
    int val = f.parseInt();
    f.close();
    if (val > 0 && val < 15) maxRingCount = val;
  }
}

void saveRingSetting(uint8_t val) {
  File f = LittleFS.open(RINGS_PATH, "w");
  f.print(val);
  f.close();
  maxRingCount = val;
}

uint8_t getRingSetting() {
  return maxRingCount;
}

// ====== Inicializace modemu ======
void modemInit() {
  SerialGSM.end();  // pro jistotu, pokud už běžel
  SerialGSM.begin(settings.baudRate, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);  // RX, TX
  //SerialGSM.begin(115200, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    delay(1000);
  sendAndPrint("AT");
    delay(200); 
  sendAndPrint("ATE1");
    delay(200); 
  sendAndPrint("AT+CMEE=2");
    delay(200); 
  sendAndPrint("AT+CLIP=1");
    delay(200);
  sendAndPrint("AT+CTZU=1");  // automatická aktualizace
    delay(200);
  sendAndPrint("AT+CTZR=1");  // či ruční dotaz
    delay(200);
}

// ====== DTR řízení ======
void setupDTR() {
  pinMode(DTR_PIN, OUTPUT);
  //digitalWrite(DTR_PIN, HIGH);
  digitalWrite(DTR_PIN, LOW);
  delay(2000);
  digitalWrite(DTR_PIN, HIGH);
}

void wakeModem() {
  digitalWrite(DTR_PIN, LOW);
  delay(2000);
  digitalWrite(DTR_PIN, HIGH);
}

// ====== Caller ID/RING zpracování ======
// Pouze parsuje jeden URC-řádek
// ====== Caller ID / RING zpracování ======
void processCallerIDLine(const String &line) {
  static bool clipSeen    = false;  // indikace, že už proběhl CLIP pro tento hovor
  static bool callLogged  = false;  // jestli už jsme ho logovali
  // 1) CLIP URC
  if (line.startsWith("+CLIP:")) {
    int f = line.indexOf('"');
    int l = line.indexOf('"', f + 1);
    if (f >= 0 && l > f) {
      String rawNum = line.substring(f + 1, l);
      String mqttNum = rawNum;
      if (mqttNum.startsWith("+")) mqttNum.remove(0, 1);

      GSM_DBG(String(F("📡 CallerID raw: ")) + rawNum);
      GSM_DBG(String(F("📡 CallerID mqtt: ")) + mqttNum);

      lastCaller = mqttNum;

      // Označíme, že už jsme CLIP viděli a začíná nové volání
      clipSeen   = true;
      ringCount  = 0;

      // Publikace na MQTT
      mqttPublishCaller(mqttNum);

      // LOG pouze jednou
      if (!callLogged) {
        logCallToFile(rawNum);      // uloží do call_log.json :contentReference[oaicite:0]{index=0}
        callLogged = true;
      }
    } else {
      GSM_DBG(F("⚠️ CLIP parse error"));
    }
  }
  // 2) RING (jen pokud jsme už viděli CLIP)
  else if (clipSeen && line == "RING") {
    ringCount = (ringCount == 0) ? 1 : (ringCount + 1);
    GSM_DBG(String(F("🔔 RING #")) + ringCount);

    // Po dosažení max. počtu vyzvánění ukončíme hovor
    if (ringCount >= maxRingCount) {
      SerialGSM.println("ATH");
      GSM_DBG(String(F("📴 ATH after ")) + ringCount + F(" rings"));
      // Resetujeme stav pro další hovory
      clipSeen    = false;
      ringCount   = 0;
      callLogged  = false;  // znovu povolíme log na příští CLIP
    }
  }
}

// Přebírá stejnou řádku a posílá ji do SMS stavového stroje
void processSmsResponseLine(const String &line) {
  // např. hledání "+CMGS:" nebo "ERROR"
  if (smsState == SMS_WAIT_OK) {
    responseBuf += line;
    if (responseBuf.indexOf("+CMGS:") != -1) {
      smsState = SMS_DONE;
    } else if (line == "ERROR") {
      smsState = SMS_ERROR;
    }
  }
}

// ====== Fronta SMS (enqueue API) ======
bool enqueueSms(const String& recipients, const String& message) {
  return smsQueue.enqueue({ recipients, message });
}

// ====== Stavový stroj pro neblokující odesílání SMS ======
void processSmsQueue() {
  unsigned long now = millis();
  if (now - lastSmsQueueStatusLog >= smsQueueStatusLogInterval) {
    GSM_DBG_FMT("SMS stav: %d, fronta má %d úkolů", smsState, smsQueue.size());
    lastSmsQueueStatusLog = now;
  }

  switch (smsState) {
    case SMS_IDLE:
      if (!smsQueue.isEmpty()) {
        currentTask = smsQueue.dequeue();
        GSM_DBG(String(F("Odesílám SMS na: ")) + currentTask.recipients);
        responseBuf.clear();
        flushSerialGSM();
        smsState = SMS_SET_TEXT_MODE;
      }
      break;

    case SMS_SET_TEXT_MODE:
      sendAndPrint("AT+CMGF=1");
      stateTimestamp = millis();
      smsState = SMS_SEND_HEADER;
      responseBuf.clear();
      break;

    case SMS_SEND_HEADER:
      // Po krátké pauze začneme posílat hlavičku AT+CMGS
      if (millis() - stateTimestamp >= CMD_INTERVAL) {
        responseBuf.clear();
        SerialGSM.print("AT+CMGS=\"");
        SerialGSM.print(currentTask.recipients);
        SerialGSM.println("\"");
        stateTimestamp = millis();
        smsState = SMS_WAIT_PROMPT;
      }
      break;

    case SMS_WAIT_PROMPT:
      // Čekáme na prompt '>' od modemu
      if (waitForPrompt(SMS_PROMPT_TIMEOUT)) {
        GSM_DBG("Prompt '>' přijat, posílám tělo zprávy");
        responseBuf.clear();
        stateTimestamp = millis();
        smsState = SMS_SEND_BODY;
      } else if (millis() - stateTimestamp >= SMS_PROMPT_TIMEOUT) {
        GSM_DBG("❌ Timeout čekání na prompt '>'");
        smsState = SMS_ERROR;
      }
      break;

    case SMS_SEND_BODY:
      // Po krátké pauze po promptu posíláme text + CTRL+Z
      if (millis() - stateTimestamp >= CMD_INTERVAL) {
        SerialGSM.print(currentTask.message);
        SerialGSM.write(26); // CTRL+Z
        stateTimestamp = millis();
        responseBuf.clear();
        smsState = SMS_WAIT_OK;
      }
      break;

    case SMS_WAIT_OK:
      // Čekáme, až modul potvrdí +CMGS: nebo vrátí ERROR
      if (millis() - stateTimestamp >= SMS_TIMEOUT) {
        GSM_DBG("❌ SMS odeslání timeout");
        smsState = SMS_ERROR;
      } else {
        // Zkontrolujeme, zda responseBuf už obsahuje "+CMGS:" nebo "ERROR"
        if (responseBuf.indexOf("+CMGS:") != -1) {
          GSM_DBG("✅ SMS odeslána (detekováno +CMGS:)");
          smsState = SMS_DONE;
        } else if (responseBuf.indexOf("ERROR") != -1) {
          GSM_DBG("❌ Modem vrátil ERROR při odesílání SMS");
          smsState = SMS_ERROR;
        }
      }
      break;

    case SMS_DONE:
      // Úspěšné odeslání: vracíme se do IDLE
      smsState = SMS_IDLE;
      break;

    case SMS_ERROR:
      // Chyba: prostě vrátíme do IDLE (můžeme sem přidat retry, logging apod.)
      smsState = SMS_IDLE;
      break;
  }
}

// ====== Blokující API ======
bool modemSendSMS(const String& recipients, const String& message) {
  flushSerialGSM();

  // 1) Nastavíme textový mód
  sendAndPrint("AT+CMGF=1");
  // Počkáme na OK (timeout 1 s)
  if (!waitForOkOrErrorOrCmgs(1000)) {
    GSM_DBG("❌ Chyba: AT+CMGF odezva ERROR nebo timeout");
    return false;
  }

  // 2) Pošleme hlavičku AT+CMGS
  responseBuf.clear();
  SerialGSM.print("AT+CMGS=\"");
  SerialGSM.print(recipients);
  SerialGSM.println("\"");

  // 3) Čekáme na prompt '>'
  if (!waitForPrompt(SMS_PROMPT_TIMEOUT)) {
    GSM_DBG("❌ Chyba: Timeout čekání na prompt '>'");
    return false;
  }

  // 4) Posíláme text a ukončíme CTRL+Z
  SerialGSM.print(message);
  SerialGSM.write(26); // CTRL+Z

  // 5) Čekáme na +CMGS: nebo ERROR
  if (!waitForOkOrErrorOrCmgs(SMS_TIMEOUT)) {
    GSM_DBG("❌ Chyba: Modem vrátil ERROR nebo timeout při odeslání těla SMS");
    return false;
  }

  GSM_DBG("✅ modemSendSMS: SMS úspěšně odeslána");
  return true;
}

bool sendSmsNow(const String& number, const String& message) {
  SmsTask task = { number, message };
  if (!smsQueue.enqueue(task)) {
    GSM_DBG("SMS fronta je plná, nelze odeslat zprávu");
    return false;
  } else {
    GSM_DBG("SMS přidána do fronty k odeslání");
    return true;
  }
}

// ====== Plánování SMS (pro případné rozšíření) ======
bool modemScheduleSMS(const String& recipients, const String& message, const String& schedule) {
  (void)schedule;
  return enqueueSms(recipients, message);
}

// ====== AT příkazy s očekávanou odpovědí ======
bool sendAtCommand(const String& cmd, const String& expected, unsigned long timeout) {
    Serial.print("> "); Serial.println(cmd); // log příkazu
    flushSerialGSM();
    SerialGSM.println(cmd);
    unsigned long t0 = millis();
    String resp;
    while (millis() - t0 < timeout) {
        if (SerialGSM.available()) {
            resp += char(SerialGSM.read());
            if (resp.indexOf(expected) != -1) {
                Serial.print("< "); Serial.println(resp); // log odpovědi
                return true;
            }
        }
    }
    Serial.print("< "); Serial.println(resp); // log i při timeoutu/ERROR
    return false;
}

// ====== Logování příchozího sériového provozu (debug) ======
void logModemData() {
  while (SerialGSM.available()) Serial.write(SerialGSM.read());
}

// Vrátí počet úloh ve frontě
size_t getSmsQueueSize() {
  return smsQueue.size();
}

// Vrátí N-tou úlohu (0 = další ke zpracování)
SmsTask getSmsQueueTask(size_t idx) {
  SmsTask empty = { "", "" };
  size_t sz = smsQueue.size();
  if (idx >= sz) return empty;
  // circular buffer: head + idx mod MAX_TASKS
  size_t pos = (smsQueue.head + idx) % MAX_TASKS;
  return smsQueue.buffer[pos];
}

// Stav N-té úlohy (pro idx>0 vždy SMS_IDLE, první bere aktuální stav stroje)
SmsState getSmsTaskState(size_t idx) {
  if (idx == 0) return smsState;
  return SMS_IDLE;
}