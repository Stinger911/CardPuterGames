#include "auth.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.hpp"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "json_util.hpp"
#include "mbedtls/sha256.h"
#include "storage.hpp"

static SemaphoreHandle_t gMtx = nullptr;
static std::vector<Session> gSess;

Lock::Lock()  { xSemaphoreTakeRecursive(gMtx, portMAX_DELAY); }
Lock::~Lock() { xSemaphoreGiveRecursive(gMtx); }

void authBegin() {
  gMtx = xSemaphoreCreateRecursiveMutex();
}

std::vector<Session>& sessionsRaw() { return gSess; }

Session* sessionFind(const std::string& token) {
  if (token.empty()) return nullptr;
  for (auto& s : gSess) if (s.token == token) return &s;
  return nullptr;
}

// ---------- утилиты ----------
static std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string hexOf(const uint8_t* b, size_t n) {
  static const char* H = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 15]; }
  return s;
}

static std::string randomHex(size_t bytes) {
  uint8_t b[32];
  if (bytes > 32) bytes = 32;
  esp_fill_random(b, bytes);
  return hexOf(b, bytes);
}

// SHA-256(salt+pass), затем 2000 раундов SHA-256(prev || pass)
static std::string hashPass(const std::string& salt, const std::string& pass) {
  uint8_t buf[32];
  std::string first = salt + pass;
  mbedtls_sha256((const unsigned char*)first.data(), first.size(), buf, 0);
  uint8_t in[32 + 64];
  size_t pl = std::min(pass.size(), (size_t)64);
  for (int i = 0; i < 2000; i++) {
    memcpy(in, buf, 32);
    memcpy(in + 32, pass.data(), pl);
    mbedtls_sha256(in, 32 + pl, buf, 0);
  }
  return hexOf(buf, 32);
}

static bool validName(const std::string& n) {
  if (n.size() < 3 || n.size() > 16) return false;
  for (char c : n) {
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

static std::string userPath(const std::string& name) {
  return SD_ROOT "/data/users/" + lower(name) + ".json";
}

// ---------- пользователи ----------
bool authRegister(const std::string& name, const std::string& pass, std::string& err) {
  if (!cfg.allowRegister) { err = "Регистрация отключена"; return false; }
  if (!sdOk()) { err = "Нет SD-карты"; return false; }
  if (!validName(name)) { err = "Имя: 3–16 символов, латиница, цифры, _ и -"; return false; }
  if (lower(name).rfind("guest", 0) == 0) { err = "Имя зарезервировано"; return false; }
  if (pass.size() < 4 || pass.size() > 32) { err = "Пароль: 4–32 символа"; return false; }
  std::string path = userPath(name);
  Lock lk;
  if (fileExists(path)) { err = "Имя занято"; return false; }
  std::string salt = randomHex(8);
  JDoc d(cJSON_CreateObject());
  cJSON_AddStringToObject(d.p, "name", name.c_str());
  cJSON_AddStringToObject(d.p, "salt", salt.c_str());
  cJSON_AddStringToObject(d.p, "hash", hashPass(salt, pass).c_str());
  cJSON_AddNumberToObject(d.p, "created", millis() / 1000);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { err = "Ошибка записи на SD"; return false; }
  std::string out = jdump(d.p);
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  if (!ok) { err = "Ошибка записи на SD"; return false; }
  return true;
}

bool authLogin(const std::string& name, const std::string& pass, std::string& canonicalName, std::string& err) {
  const char* bad = "Неверное имя или пароль";
  if (!validName(name) || !sdOk()) { err = bad; return false; }
  std::string txt;
  if (!readFile(userPath(name), txt, 2048)) { err = bad; return false; }
  JDoc d(cJSON_Parse(txt.c_str()));
  if (!d.p) { err = bad; return false; }
  std::string want = jstr(d.p, "hash");
  std::string got = hashPass(jstr(d.p, "salt"), pass);
  // сравнение без раннего выхода
  uint8_t diff = (got.size() != want.size());
  for (size_t i = 0; i < std::min(got.size(), want.size()); i++) diff |= got[i] ^ want[i];
  if (diff) { err = bad; return false; }
  canonicalName = jstr(d.p, "name");
  return true;
}

// ---------- сессии ----------
std::string guestName() {
  Lock lk;
  for (int tries = 0; tries < 20; tries++) {
    std::string n = "Guest" + std::to_string(1000 + (esp_random() % 9000));
    bool taken = false;
    for (auto& s : gSess) if (s.name == n) { taken = true; break; }
    if (!taken) return n;
  }
  return "Guest" + std::to_string(esp_random() % 100000);
}

std::string sessionCreate(const std::string& name, bool guest) {
  Lock lk;
  // повторный вход того же пользователя заменяет старую сессию
  std::string ln = lower(name);
  for (auto it = gSess.begin(); it != gSess.end(); ++it) {
    if (lower(it->name) == ln) { gSess.erase(it); break; }
  }
  if (gSess.size() >= MAX_SESSIONS) return "";
  Session s;
  s.token = randomHex(16);
  s.name = name;
  s.guest = guest;
  s.lastSeen = millis();
  gSess.push_back(s);
  return s.token;
}

bool sessionGet(const std::string& token, Session& out, bool touch) {
  Lock lk;
  Session* s = sessionFind(token);
  if (!s) return false;
  if (touch) s->lastSeen = millis();
  out = *s;
  return true;
}

void sessionRemove(const std::string& token) {
  Lock lk;
  for (auto it = gSess.begin(); it != gSess.end(); ++it) {
    if (it->token == token) { gSess.erase(it); return; }
  }
}
