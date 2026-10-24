#include "storage.h"
#include "config.h"
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

AppConfig cfg;

static bool gSd = false;
static String gGamesCache;
static uint32_t gGamesAt = 0;
static int gGamesN = 0;

bool storageBegin() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  gSd = SD.begin(SD_CS, SPI, 25000000);
  if (gSd) {
    SD.mkdir(SD_ROOT);
    SD.mkdir(SD_ROOT "/games");
    SD.mkdir(SD_ROOT "/data");
    SD.mkdir(SD_ROOT "/data/users");
  }
  return gSd;
}

bool sdOk() { return gSd; }

uint32_t sdFreeMB() {
  if (!gSd) return 0;
  return (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
}

void loadConfig() {
  if (!gSd) return;
  File f = SD.open(SD_ROOT "/config.json");
  if (!f) return;
  JsonDocument d;
  if (!deserializeJson(d, f)) {
    if (d["ssid"].is<const char*>())        cfg.ssid = d["ssid"].as<String>();
    if (d["pass"].is<const char*>())        cfg.pass = d["pass"].as<String>();
    if (d["channel"].is<int>())             cfg.channel = constrain((int)d["channel"], 1, 13);
    if (d["max_clients"].is<int>())         cfg.maxClients = constrain((int)d["max_clients"], 1, 10);
    if (d["allow_guests"].is<bool>())       cfg.allowGuests = d["allow_guests"];
    if (d["allow_register"].is<bool>())     cfg.allowRegister = d["allow_register"];
  }
  f.close();
  // WPA2 требует пароль >= 8 символов, иначе делаем сеть открытой
  if (cfg.pass.length() > 0 && cfg.pass.length() < 8) cfg.pass = "";
}

bool validId(const String& s) {
  if (s.length() == 0 || s.length() > 32) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

String gamesJson(bool force) {
  if (!force && gGamesCache.length() && millis() - gGamesAt < 10000) return gGamesCache;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  gGamesN = 0;
  if (gSd) {
    File root = SD.open(SD_ROOT "/games");
    if (root && root.isDirectory()) {
      for (File d = root.openNextFile(); d; d = root.openNextFile()) {
        bool isDir = d.isDirectory();
        String id = String(d.name());
        d.close();
        if (!isDir || !validId(id)) continue;
        if (!SD.exists(SD_ROOT "/games/" + id + "/index.html")) continue;
        JsonObject g = arr.add<JsonObject>();
        g["id"] = id;
        g["title"] = id;
        g["description"] = "";
        g["min"] = 1;
        g["max"] = 1;
        File m = SD.open(SD_ROOT "/games/" + id + "/manifest.json");
        if (m) {
          JsonDocument md;
          if (!deserializeJson(md, m)) {
            if (md["title"].is<const char*>())       g["title"] = md["title"].as<String>();
            if (md["description"].is<const char*>()) g["description"] = md["description"].as<String>();
            if (md["icon"].is<const char*>())        g["icon"] = md["icon"].as<String>();
            int mn = md["min"] | 1, mx = md["max"] | 1;
            mn = constrain(mn, 1, 8);
            mx = constrain(mx, mn, 8);
            g["min"] = mn;
            g["max"] = mx;
          }
          m.close();
        }
        gGamesN++;
      }
      root.close();
    }
  }
  gGamesCache = "";
  serializeJson(doc, gGamesCache);
  gGamesAt = millis();
  return gGamesCache;
}

int gamesCount() { gamesJson(); return gGamesN; }

int gameMax(const String& id) {
  if (!gSd || !validId(id)) return 0;
  if (!SD.exists(SD_ROOT "/games/" + id + "/index.html")) return 0;
  int mx = 1;
  File m = SD.open(SD_ROOT "/games/" + id + "/manifest.json");
  if (m) {
    JsonDocument md;
    if (!deserializeJson(md, m)) {
      int mn = constrain((int)(md["min"] | 1), 1, 8);
      mx = constrain((int)(md["max"] | 1), mn, 8);
    }
    m.close();
  }
  return mx;
}
