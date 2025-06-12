#pragma once
#include <Arduino.h>

/**
 * Spustí jednorázový NTP požadavek. 
 * Pak volejte v loop() ntpIsSynced(), dokud nevrátí true.
 */
void ntpBegin();

/**
 * Zpracuje příchozí UDP odpověď. 
 * @return true jakmile je čas jednou úspěšně nastaven.
 */
bool ntpIsSynced();

/**
 * Vrátí nastavený čas formou "YYYY-MM-DD HH:MM:SS".
 * Zavolat až po tom, co ntpIsSynced() jednou vrátilo true.
 */
String ntpGetTimeString();
