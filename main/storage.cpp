#include "storage.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <mutex>

#include "config.hpp"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "json_util.hpp"
#include "sdmmc_cmd.h"

static const char* TAG = "storage";

AppConfig cfg;

static bool gSd = false;
static std::string gGamesCache;
static uint32_t gGamesAt = 0;
static int gGamesN = 0;
static std::mutex gGamesMtx;

static void mkdirp(const char* p) {
  if (mkdir(p, 0775) != 0 && errno != EEXIST) ESP_LOGW(TAG, "mkdir %s: errno %d", p, errno);
}

bool storageBegin() {
  const spi_host_device_t host_id = SPI3_HOST;   // LCD сидит на другой шине
  const int freqs[] = {25000, 20000};
  for (int khz : freqs) {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI;
    bus.miso_io_num = SD_MISO;
    bus.sclk_io_num = SD_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t err = spi_bus_initialize(host_id, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
      return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = host_id;
    host.max_freq_khz = khz;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = static_cast<gpio_num_t>(SD_CS);
    slot.host_id = host_id;
    esp_vfs_fat_mount_config_t mnt = {};
    mnt.format_if_mount_failed = false;
    mnt.max_files = 8;
    mnt.allocation_unit_size = 16 * 1024;
    sdmmc_card_t* card = nullptr;
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mnt, &card);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "SD mounted at %d kHz", (int)card->real_freq_khz);
      gSd = true;
      break;
    }
    ESP_LOGE(TAG, "SD mount at %d kHz: %s", khz, esp_err_to_name(err));
    spi_bus_free(host_id);
  }
  if (gSd) {
    mkdirp(SD_ROOT);
    mkdirp(SD_ROOT "/games");
    mkdirp(SD_ROOT "/data");
    mkdirp(SD_ROOT "/data/users");
  }
  return gSd;
}

bool sdOk() { return gSd; }

uint32_t sdFreeMB() {
  if (!gSd) return 0;
  uint64_t total = 0, free_bytes = 0;
  if (esp_vfs_fat_info(SD_MOUNT, &total, &free_bytes) != ESP_OK) return 0;
  return static_cast<uint32_t>(free_bytes / (1024ULL * 1024ULL));
}

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool readFile(const std::string& path, std::string& out, size_t maxLen) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    out.append(buf, n);
    if (out.size() > maxLen) { fclose(f); return false; }
  }
  fclose(f);
  return true;
}

void loadConfig() {
  if (!gSd) return;
  std::string txt;
  if (!readFile(SD_ROOT "/config.json", txt)) return;
  JDoc d(cJSON_Parse(txt.c_str()));
  if (d.p) {
    auto isStr = [&](const char* k) { return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(d.p, k)); };
    auto isNum = [&](const char* k) { return cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(d.p, k)); };
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (isStr("ssid"))     cfg.ssid = jstr(d.p, "ssid");
    if (isStr("pass"))     cfg.pass = jstr(d.p, "pass");
    if (isNum("channel"))     cfg.channel = clampi(jint(d.p, "channel"), 1, 13);
    if (isNum("max_clients")) cfg.maxClients = clampi(jint(d.p, "max_clients"), 1, 10);
    cfg.allowGuests = jbool(d.p, "allow_guests", cfg.allowGuests);
    cfg.allowRegister = jbool(d.p, "allow_register", cfg.allowRegister);
  }
  // WPA2 требует пароль >= 8 символов, иначе делаем сеть открытой
  if (cfg.pass.size() > 0 && cfg.pass.size() < 8) cfg.pass = "";
  if (cfg.ssid.empty() || cfg.ssid.size() > 32) cfg.ssid = "CardPutter-Games";
}

bool validId(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (char c : s) {
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
  }
  return true;
}

static void readManifest(const std::string& id, int& mn, int& mx, cJSON* into) {
  mn = 1; mx = 1;
  std::string txt;
  if (!readFile(SD_ROOT "/games/" + id + "/manifest.json", txt, 4096)) return;
  JDoc md(cJSON_Parse(txt.c_str()));
  if (!md.p) return;
  auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  mn = clampi(jint(md.p, "min", 1), 1, 8);
  mx = clampi(jint(md.p, "max", 1), mn, 8);
  if (into) {
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(md.p, "title")))
      cJSON_ReplaceItemInObject(into, "title", cJSON_CreateString(jstr(md.p, "title").c_str()));
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(md.p, "description")))
      cJSON_ReplaceItemInObject(into, "description", cJSON_CreateString(jstr(md.p, "description").c_str()));
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(md.p, "icon")))
      cJSON_AddStringToObject(into, "icon", jstr(md.p, "icon").c_str());
  }
}

std::string gamesJson(bool force) {
  std::lock_guard<std::mutex> lk(gGamesMtx);
  if (!force && !gGamesCache.empty() && millis() - gGamesAt < 10000) return gGamesCache;
  JDoc arr(cJSON_CreateArray());
  gGamesN = 0;
  if (gSd) {
    DIR* dir = opendir(SD_ROOT "/games");
    if (dir) {
      while (struct dirent* e = readdir(dir)) {
        std::string id = e->d_name;
        if (!validId(id) || !isDir(SD_ROOT "/games/" + id)) continue;
        if (!fileExists(SD_ROOT "/games/" + id + "/index.html")) continue;
        cJSON* g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "id", id.c_str());
        cJSON_AddStringToObject(g, "title", id.c_str());
        cJSON_AddStringToObject(g, "description", "");
        int mn, mx;
        readManifest(id, mn, mx, g);
        cJSON_AddNumberToObject(g, "min", mn);
        cJSON_AddNumberToObject(g, "max", mx);
        cJSON_AddItemToArray(arr.p, g);
        gGamesN++;
      }
      closedir(dir);
    }
  }
  gGamesCache = jdump(arr.p);
  gGamesAt = millis();
  return gGamesCache;
}

int gamesCount() { gamesJson(); return gGamesN; }

int gameMax(const std::string& id) {
  if (!gSd || !validId(id)) return 0;
  if (!fileExists(SD_ROOT "/games/" + id + "/index.html")) return 0;
  int mn, mx;
  readManifest(id, mn, mx, nullptr);
  return mx;
}
