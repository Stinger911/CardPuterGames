#include <M5Cardputer.h>
#include <WiFi.h>
#include <DNSServer.h>
#include "config.h"
#include "storage.h"
#include "auth.h"
#include "lobby.h"
#include "web.h"

static DNSServer dns;
static M5Canvas canvas(&M5Cardputer.Display);
static int page = 0;               // 0 статус, 1 игроки, 2 комнаты
static uint32_t lastUi = 0, lastTick = 0;

static void startAP() {
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(cfg.ssid.c_str(), cfg.pass.length() ? cfg.pass.c_str() : nullptr, cfg.channel, 0, cfg.maxClients);
  WiFi.setSleep(false);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", ip);          // любой домен -> 192.168.4.1 (captive portal)
}

static void drawUi() {
  canvas.fillSprite(TFT_BLACK);
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_CYAN);
  canvas.setCursor(2, 2);
  canvas.printf("%s v%s  [%d/3]", FW_NAME, FW_VERSION, page + 1);
  canvas.setTextColor(TFT_WHITE);
  int y = 16;
  auto line = [&](const String& s, uint16_t col = TFT_WHITE) {
    canvas.setTextColor(col);
    canvas.setCursor(2, y);
    canvas.print(s);
    y += 12;
  };
  int players, rooms;
  lobbyStats(players, rooms);

  if (page == 0) {
    line("WiFi: " + cfg.ssid, TFT_GREEN);
    line(String("Auth: ") + (cfg.pass.length() ? "WPA2" : "open") + "  ch" + String(cfg.channel));
    line("URL:  http://" AP_IP_STR);
    line("Clients: " + String(WiFi.softAPgetStationNum()) + "/" + String(cfg.maxClients));
    line("Players: " + String(players) + "  Rooms: " + String(rooms));
    if (sdOk()) line("SD: " + String(sdFreeMB()) + " MB free, games: " + String(gamesCount()), TFT_WHITE);
    else        line("SD: NOT FOUND!", TFT_RED);
    line("Heap: " + String(ESP.getFreeHeap() / 1024) + " KB");
  } else if (page == 1) {
    auto v = lobbyPlayerLines();
    if (v.empty()) line("No players", TFT_DARKGREY);
    for (size_t i = 0; i < v.size() && i < 9; i++) line(v[i]);
  } else {
    auto v = lobbyRoomLines();
    if (v.empty()) line("No rooms", TFT_DARKGREY);
    for (size_t i = 0; i < v.size() && i < 9; i++) line(v[i]);
  }
  canvas.setTextColor(TFT_DARKGREY);
  canvas.setCursor(2, 124);
  canvas.print("1/2/3 page   r rescan games");
  canvas.pushSprite(0, 0);
}

void setup() {
  auto mc = M5.config();
  M5Cardputer.begin(mc, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(80);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());

  authBegin();                 // мьютекс создаём до всего остального
  storageBegin();
  loadConfig();
  startAP();
  webBegin();
  drawUi();
}

void loop() {
  dns.processNextRequest();
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto st = M5Cardputer.Keyboard.keysState();
    for (char c : st.word) {
      if (c == '1') page = 0;
      else if (c == '2') page = 1;
      else if (c == '3') page = 2;
      else if (c == 'r') gamesJson(true);
    }
    drawUi();
  }

  uint32_t now = millis();
  if (now - lastTick > 2000) {
    lastTick = now;
    lobbyTick();
    lobbyCleanupWs();
  }
  if (now - lastUi > 1000) {
    lastUi = now;
    drawUi();
  }
  delay(2);
}
