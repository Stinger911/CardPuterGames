#include "auth.h"
#include "config.h"
#include "storage.h"
#include <SD.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>

static SemaphoreHandle_t gMtx = nullptr;
static std::vector<Session> gSess;

Lock::Lock()  { xSemaphoreTakeRecursive(gMtx, portMAX_DELAY); }
Lock::~Lock() { xSemaphoreGiveRecursive(gMtx); }

void authBegin() {
  gMtx = xSemaphoreCreateRecursiveMutex();
}

std::vector<Session>& sessionsRaw() { return gSess; }

Session* sessionFind(const String& token) {
  if (!token.length()) return nullptr;
  for (auto& s : gSess) if (s.token == token) return &s;
  return nullptr;
}

// ---------- утилиты ----------
static String hexOf(const uint8_t* b, size_t n) {
  static const char* H = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 15]; }
  return s;
}

static String randomHex(size_t bytes) {
  uint8_t b[32];
  if (bytes > 32) bytes = 32;
  for (size_t i = 0; i < bytes; i += 4) {
    uint32_t r = esp_random();
    for (size_t j = 0; j < 4 && i + j < bytes; j++) b[i + j] = (r >> (8 * j)) & 0xFF;
  }
  return hexOf(b, bytes);
}

// SHA-256(salt+pass), затем 2000 раундов SHA-256(prev || pass)
static String hashPass(const String& salt, const String& pass) {
  uint8_t buf[32];
  String first = salt + pass;
  mbedtls_sha256((const unsigned char*)first.c_str(), first.length(), buf, 0);
  uint8_t in[32 + 64];
  size_t pl = min((size_t)pass.length(), (size_t)64);
  for (int i = 0; i < 2000; i++) {
    memcpy(in, buf, 32);
    memcpy(in + 32, pass.c_str(), pl);
    mbedtls_sha256(in, 32 + pl, buf, 0);
  }
  return hexOf(buf, 32);
}

static bool validName(const String& n) {
  if (n.length() < 3 || n.length() > 16) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

static String userPath(const String& name) {
  String l = name;
  l.toLowerCase();
  return "/data/users/" + l + ".json";
}

// ---------- пользователи ----------
bool authRegister(const String& name, const String& pass, String& err) {
  if (!cfg.allowRegister) { err = "Регистрация отключена"; return false; }
  if (!sdOk()) { err = "Нет SD-карты"; return false; }
  if (!validName(name)) { err = "Имя: 3–16 символов, латиница, цифры, _ и -"; return false; }
  String l = name; l.toLowerCase();
  if (l.startsWith("guest")) { err = "Имя зарезервировано"; return false; }
  if (pass.length() < 4 || pass.length() > 32) { err = "Пароль: 4–32 символа"; return false; }
  String path = userPath(name);
  Lock lk;
  if (SD.exists(path)) { err = "Имя занято"; return false; }
  String salt = randomHex(8);
  JsonDocument d;
  d["name"] = name;
  d["salt"] = salt;
  d["hash"] = hashPass(salt, pass);
  d["created"] = (uint32_t)(millis() / 1000);
  File f = SD.open(path, FILE_WRITE);
  if (!f) { err = "Ошибка записи на SD"; return false; }
  serializeJson(d, f);
  f.close();
  return true;
}

bool authLogin(const String& name, const String& pass, String& canonicalName, String& err) {
  if (!validName(name) || !sdOk()) { err = "Неверное имя или пароль"; return false; }
  File f = SD.open(userPath(name));
  if (!f) { err = "Неверное имя или пароль"; return false; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) { err = "Неверное имя или пароль"; return false; }
  String salt = d["salt"].as<String>();
  String want = d["hash"].as<String>();
  String got = hashPass(salt, pass);
  // сравнение без раннего выхода
  uint8_t diff = (got.length() != want.length());
  for (size_t i = 0; i < min(got.length(), want.length()); i++) diff |= got[i] ^ want[i];
  if (diff) { err = "Неверное имя или пароль"; return false; }
  canonicalName = d["name"].as<String>();
  return true;
}

// ---------- сессии ----------
String guestName() {
  Lock lk;
  for (int tries = 0; tries < 20; tries++) {
    String n = "Guest" + String(1000 + (esp_random() % 9000));
    bool taken = false;
    for (auto& s : gSess) if (s.name == n) { taken = true; break; }
    if (!taken) return n;
  }
  return "Guest" + String(esp_random() % 100000);
}

String sessionCreate(const String& name, bool guest) {
  Lock lk;
  // повторный вход того же пользователя заменяет старую сессию
  for (auto it = gSess.begin(); it != gSess.end(); ++it) {
    if (it->name.equalsIgnoreCase(name)) { gSess.erase(it); break; }
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

bool sessionGet(const String& token, Session& out, bool touch) {
  Lock lk;
  Session* s = sessionFind(token);
  if (!s) return false;
  if (touch) s->lastSeen = millis();
  out = *s;
  return true;
}

void sessionRemove(const String& token) {
  Lock lk;
  for (auto it = gSess.begin(); it != gSess.end(); ++it) {
    if (it->token == token) { gSess.erase(it); return; }
  }
}
