#pragma once
#include <Arduino.h>

#define FW_NAME    "CardPutter Games"
#define FW_VERSION "0.1.0"

// SD-карта Cardputer (отдельная SPI-шина)
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12

#define AP_IP_STR "192.168.4.1"

#define MAX_SESSIONS   24
#define MAX_ROOMS      8
#define SESSION_IDLE_MS (15UL * 60UL * 1000UL)   // сессия без активности
#define WS_GRACE_MS     20000UL                  // сколько держим игрока в комнате после обрыва WS (переход портал -> игра)

struct AppConfig {
  String  ssid         = "CardPutter-Games";
  String  pass         = "";        // пусто = открытая сеть (нужно для автопоказа captive portal)
  uint8_t channel      = 6;
  uint8_t maxClients   = 8;
  bool    allowGuests  = true;
  bool    allowRegister = true;
};

extern AppConfig cfg;
