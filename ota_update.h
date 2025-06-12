// ota_update.h
#pragma once
#include <Arduino.h>
#include <Ethernet.h>

// inicializace (stávající)
void otaInit();

// hlavní handler pro /ota (FW i FS upload)
bool otaHandle(EthernetClient &client, const String& method, const String& path);

