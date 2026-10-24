#pragma once
#include <cstdint>
#include <string>

bool        storageBegin();                     // монтирует SD
bool        sdOk();
void        loadConfig();                       // /l18games/config.json (необязательный)
std::string gamesJson(bool force = false);      // массив игр из /l18games/games/*/manifest.json (кэш 10 с)
int         gameMax(const std::string& id);     // макс. игроков по манифесту; 0 если игры нет
bool        validId(const std::string& s);      // [A-Za-z0-9_-]{1,32}
int         gamesCount();
uint32_t    sdFreeMB();
bool        readFile(const std::string& path, std::string& out, size_t maxLen = 16384);
bool        fileExists(const std::string& path);
bool        isDir(const std::string& path);
