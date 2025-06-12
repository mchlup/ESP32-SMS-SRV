// ota_update.cpp
#include "ota_update.h"
#include <Update.h>
#include <LittleFS.h>
#include <Arduino.h>
#include <Ethernet.h>

static const char* otaPage = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><title>OTA/FS Upload</title></head>
<body><h2>OTA Firmware Update nebo FS Upload</h2>
<form method="POST" action="/ota" enctype="multipart/form-data">
  <label>Firmware (.bin): <input type="file" name="firmware" accept=".bin"></label><br><br>
  <label>Soubor na FS:   <input type="file" name="fsfile"></label><br><br>
  <button type="submit">Nahrát</button>
</form></body></html>
)rawliteral";

// čte řádek (LF-terminated)
static String readLine(EthernetClient &c) {
  return c.readStringUntil('\n');
}

void otaInit() {
  // žádná speciální inicializace
}

bool otaHandle(EthernetClient &client, const String& method, const String& path) {  
  if (path != "/ota") return false;

  // GET → formulář
  if (method == "GET") {
    client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "Connection: close\r\n\r\n"
    );
    client.print(otaPage);
    delay(1);
    client.stop();
    return true;
  }

  // POST → multipart parsování
  // 1) načti hlavičky až po prázdný řádek
  String line;
  int contentLength = 0;
  String boundary;
  while (client.connected()) {
    line = readLine(client);
    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    }
    else if (line.startsWith("Content-Type: multipart/form-data")) {
      int b = line.indexOf("boundary=");
      if (b != -1) {
        boundary = "--" + line.substring(b + 9);
        boundary.trim();
      }
    }
    if (line == "\r" || line == "") break;
  }
  if (boundary.isEmpty() || contentLength <= 0) {
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nChybí boundary nebo Content-Length");
    client.stop();
    return true;
  }

  // 2) projdi části
  enum Mode { NONE, OTA, FS } mode = NONE;
  String filename;
  bool otaDone = false;
  size_t toRead = contentLength;
  const size_t bufSize = 512;
  uint8_t buf[bufSize];

  while (!otaDone && toRead > 0) {
    String hdr = readLine(client);
    toRead -= hdr.length() + 1;
    if (!hdr.startsWith(boundary)) continue;
    if (hdr.endsWith("--")) break; // konec multipart

    // Content-Disposition
    String disp = readLine(client);
    toRead -= disp.length() + 1;
    int fn = disp.indexOf("filename=\"");
    if (fn == -1) break;
    int fn2 = disp.indexOf("\"", fn + 10);
    filename = disp.substring(fn + 10, fn2);

    // skip zbylé hlavičky části
    while (true) {
      String l2 = readLine(client);
      toRead -= l2.length() + 1;
      if (l2 == "\r" || l2 == "") break;
    }

    if (filename.endsWith(".bin")) {
      // OTA firmware
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nOTA begin failed");
        client.stop();
        return true;
      }
      mode = OTA;
      // čti firmware data
      while (toRead > 0) {
        int r = client.read(buf, min(bufSize, toRead));
        if (r <= 0) break;
        toRead -= r;
        // boundary check
        String chunk((char*)buf, r);
        int bi = chunk.indexOf(boundary);
        if (bi >= 0) {
          Update.write(buf, bi - 2); // odstranit CRLF před boundary
          otaDone = true;
          break;
        }
        Update.write(buf, r);
      }
    } else {
      // FS upload (jeden File f)
      File f = LittleFS.open("/" + filename, "w");
      if (!f) {
        client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nNelze otevřít " + filename);
        client.stop();
        return true;
      }
      mode = FS;
      bool fsDone = false;
      // čti dokud nejsi na další boundary
      while (!fsDone && client.connected()) {
        unsigned long start = millis();
        while (!client.available() && (millis() - start < 1000)) { /* timeout */ }
        if (!client.available()) break;
        int len = client.read(buf, bufSize);
        if (len <= 0) break;
        String chk((char*)buf, min((int)boundary.length(), len));
        if (chk.startsWith(boundary)) {
          fsDone = true;
        } else {
          f.write(buf, len);
        }
      }
      f.close();
      // pokračuj k další části
      continue;
    }
  }

  // 3) dokonči a odpověz
  if (mode == OTA) {
    if (Update.end(true)) {
      client.print("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOTA OK, restartuji...");
      delay(200);
      ESP.restart();
    } else {
      client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nOTA failed");
    }
    client.stop();
    return true;
  }

  client.print("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nUpload FS OK");
  client.stop();
  return true;
}
