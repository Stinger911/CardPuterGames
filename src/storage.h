#pragma once
#include <Arduino.h>

bool   storageBegin();            // монтирует SD
bool   sdOk();
void   loadConfig();              // /config.json (необязательный)
String gamesJson(bool force = false);   // массив игр из /games/*/manifest.json (кэш 10 с)
int    gameMax(const String& id); // макс. игроков по манифесту; 0 если игры нет
bool   validId(const String& s);  // [A-Za-z0-9_-]{1,32}
int    gamesCount();
uint32_t sdFreeMB();
