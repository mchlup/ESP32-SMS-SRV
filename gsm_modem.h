// gsm_modem.h (upravená verze)
// původní obsah viz :contentReference[oaicite:0]{index=0}

#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>

extern HardwareSerial SerialGSM;

// ======= Pinout GSM modemu (uprav dle hw) =======
#define GSM_RX_PIN   16  // RX2 → modul TX
#define GSM_TX_PIN   17  // TX2 → modul RX
#define GSM_PWR_PIN   4  // PWRKEY (volitelně, nepoužito v cpp)
#define DTR_PIN      25  // GPIO ESP32 pro ovládání DTR RS232 (probuzení modemu)

// ======= Stavový automat pro odesílání SMS =======
enum SmsState {
  SMS_IDLE,
  SMS_SET_TEXT_MODE,
  SMS_SEND_HEADER,
  SMS_WAIT_PROMPT,
  SMS_SEND_BODY,
  SMS_WAIT_OK,
  SMS_DONE,
  SMS_ERROR
};

// ====== Datové struktury fronty ======
struct SmsTask {
  String recipients;
  String message;
};

size_t getSmsQueueSize();
SmsTask getSmsQueueTask(size_t idx);
SmsState getSmsTaskState(size_t idx);

// ======= API pro práci s GSM modemem =======
void modemInit();

// Neblokující fronta + stavový stroj
bool enqueueSms(const String& recipients, const String& message);
void processSmsQueue();
void processSmsResponseLine(const String &line);
void processCallerIDLine(const String &line);
void handleModemURC();

// Blokující jednorázové API (pro testování)
bool modemSendSMS(const String& recipients, const String& message);
bool sendSmsNow(const String& number, const String& message);

// Vytiskne základní nastavení modemu na Serial
void printModemSettings();

// AT příkaz s očekávanou odpovědí (pro debug, pokročilé použití)
bool sendAtCommand(const String& cmd, const String& expected, unsigned long timeout = 1000);

// Plánování SMS (možnost rozšíření do budoucna)
bool modemScheduleSMS(const String& recipients, const String& message, const String& schedule);

// Výpis všech dat ze sériové linky (pro debug)
void logModemData();

// ======= Správa volání, CLIP logování, RING nastavení =======
void logCallToFile(const String& caller);
void loadRingSetting();
void saveRingSetting(uint8_t val);
uint8_t getRingSetting();

// ======= Časové funkce =======
String getCurrentTimeString();
//void syncTimeAndDebug();           // Synchronizace přes NTP (pokud je)

// ======= DTR řízení (pro probuzení modemu) =======
void setupDTR();
void wakeModem();
//void startNtpSync();
//void processNtpSync();

// ======= **Nové deklarace pro SMS historii** =======
uint16_t getSmsHistoryMaxCount();
void setSmsHistoryMaxCount(uint16_t v);
void loadSmsHistoryMaxCount();

// ======= **Nové deklarace pro stav modemu** =======
uint8_t readSignalQuality();
String readOperatorName();
