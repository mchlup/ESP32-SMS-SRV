#include "ntp_sync.h"
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Dns.h>          // pro DNSClient
#include <time.h>

#define NTP_PACKET_SIZE 48
static EthernetUDP udp;
static DNSClient dns;      // <— tady
static byte packetBuffer[NTP_PACKET_SIZE];
static bool synced = false;
static unsigned long lastSent = 0;

static const char*  ntpServerName = "pool.ntp.org";
static const uint16_t ntpPort     = 123;
static const uint16_t localPort   = 2390;
static const unsigned long RETRY_MS = 10000UL;
static const unsigned long SEVENTY_YEARS = 2208988800UL;

static void sendNtpRequest() {
  udp.begin(localPort);

  IPAddress ntpIP;
  // DNS lookup přes DNSClient
  dns.begin(Ethernet.dnsServerIP());
  if (dns.getHostByName(ntpServerName, ntpIP) != 1) {
    // DNS selhalo, čekáme na retry
    lastSent = millis();
    return;
  }

  // připravíme NTP packet
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;  // LI, Version, Mode

  udp.beginPacket(ntpIP, ntpPort);
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
  lastSent = millis();
}

void ntpBegin() {
  synced = false;
  lastSent = 0;
  // „CET-1CEST,M3.5.0/2,M10.5.0/3“ znamená:
  // CET = standard (UTC+1), CEST = summer (UTC+2),
  // od třetího nedělního března v 2:00 = přechod na summer,
  // od pátého nedělního října v 3:00 = návrat na standard.
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
  sendNtpRequest();
}

bool ntpIsSynced() {
  if (synced) return true;

  int size = udp.parsePacket();
  if (size >= NTP_PACKET_SIZE) {
    udp.read(packetBuffer, NTP_PACKET_SIZE);
    unsigned long highWord       = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord        = word(packetBuffer[42], packetBuffer[43]);
    unsigned long secsSince1900  = (highWord << 16) | lowWord;
    unsigned long epoch          = secsSince1900 - SEVENTY_YEARS;

    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);

    synced = true;
    return true;
  }

  if (millis() - lastSent >= RETRY_MS) {
    sendNtpRequest();
  }
  return false;
}

String ntpGetTimeString() {
  time_t now = time(nullptr);
  struct tm tminfo;
  localtime_r(&now, &tminfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tminfo);
  return String(buf);
}
