#include <cstdio>
#include <string>

#include "auth.hpp"
#include "config.hpp"
#include "lcd.hpp"
#include "dns.hpp"
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.hpp"
#include "lobby.hpp"
#include "nvs_flash.h"
#include "storage.hpp"
#include "web.hpp"

static const char* TAG = "main";
static int page = 0;   // 0 статус, 1 игроки, 2 комнаты

static void startAP() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* ap = esp_netif_create_default_wifi_ap();   // 192.168.4.1/24 по умолчанию

  wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wic));

  wifi_config_t wc = {};
  strlcpy(reinterpret_cast<char*>(wc.ap.ssid), cfg.ssid.c_str(), sizeof wc.ap.ssid);
  wc.ap.ssid_len = cfg.ssid.size();
  wc.ap.channel = cfg.channel;
  wc.ap.max_connection = cfg.maxClients;
  if (cfg.pass.empty()) {
    wc.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    strlcpy(reinterpret_cast<char*>(wc.ap.password), cfg.pass.c_str(), sizeof wc.ap.password);
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_ps(WIFI_PS_NONE);

  // DHCP раздаёт себя же как DNS: любой домен -> 192.168.4.1 (captive portal)
  esp_netif_dns_info_t dns = {};
  esp_netif_str_to_ip4(AP_IP_STR, &dns.ip.u_addr.ip4);
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dhcps_offer_t offer = OFFER_DNS;
  esp_netif_dhcps_stop(ap);
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
  esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
  esp_netif_dhcps_start(ap);
}

static int stationCount() {
  wifi_sta_list_t l;
  return esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

static void drawUi() {
  using namespace gfx;
  display_frame([] {
    char hdr[48];
    snprintf(hdr, sizeof hdr, "%s V%s  [%d/3]", FW_NAME, FW_VERSION, page + 1);
    text(hdr, 2, 2, Cyan);
    int y = 16;
    auto line = [&](const std::string& s, uint16_t col = White) {
      text(s.c_str(), 2, y, col);
      y += 12;
    };
    int players, rooms;
    lobbyStats(players, rooms);

    if (page == 0) {
      line("WiFi: " + cfg.ssid, Green);
      line(std::string("Auth: ") + (cfg.pass.empty() ? "open" : "WPA2") + "  ch" + std::to_string(cfg.channel));
      line("URL:  http://" AP_IP_STR);
      line("Clients: " + std::to_string(stationCount()) + "/" + std::to_string(cfg.maxClients));
      line("Players: " + std::to_string(players) + "  Rooms: " + std::to_string(rooms));
      if (sdOk()) line("SD: " + std::to_string(sdFreeMB()) + " MB free, games: " + std::to_string(gamesCount()));
      else        line("SD: NOT FOUND!", Red);
      line("Heap: " + std::to_string(esp_get_free_heap_size() / 1024) + " KB");
    } else {
      auto v = page == 1 ? lobbyPlayerLines() : lobbyRoomLines();
      if (v.empty()) line(page == 1 ? "No players" : "No rooms", Grey);
      for (size_t i = 0; i < v.size() && i < 9; i++) line(v[i]);
    }
    text("1/2/3 page   r rescan games", 2, 124, Grey);
  });
}

extern "C" void app_main() {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    e = nvs_flash_init();
  }
  ESP_ERROR_CHECK(e);

  display_init();

  // Внутренняя I2C-шина Cardputer ADV (клавиатура TCA8418)
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0;
  bc.sda_io_num = GPIO_NUM_8;
  bc.scl_io_num = GPIO_NUM_9;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) ESP_LOGE(TAG, "I2C bus init failed");

  authBegin();                 // мьютекс создаём до всего остального
  storageBegin();
  loadConfig();
  startAP();
  dnsStart();
  webBegin();
  keyboard_start(bus);
  drawUi();
  ESP_LOGI(TAG, "up: ssid '%s', http://" AP_IP_STR, cfg.ssid.c_str());

  uint32_t lastUi = 0, lastTick = 0;
  while (true) {
    bool redraw = false;
    for (KeyAction a; (a = keyboard_next()) != KeyAction::None;) {
      if (a == KeyAction::Page1) page = 0;
      else if (a == KeyAction::Page2) page = 1;
      else if (a == KeyAction::Page3) page = 2;
      else if (a == KeyAction::Rescan) gamesJson(true);
      redraw = true;
    }
    uint32_t now = millis();
    if (now - lastTick > 2000) {
      lastTick = now;
      storageRetry();
      lobbyTick();
    }
    if (redraw || now - lastUi > 1000) {
      lastUi = now;
      drawUi();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
