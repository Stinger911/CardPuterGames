#include "lobby.hpp"

#include <unistd.h>

#include <algorithm>

#include "auth.hpp"
#include "config.hpp"
#include "esp_log.h"
#include "json_util.hpp"
#include "storage.hpp"

static const char* TAG = "lobby";

struct Room {
  uint16_t id;
  std::string game;
  std::string title;
  uint8_t maxP;
  std::string ownerTok;
  std::vector<std::string> members;   // токены
  bool open = true;                   // false после старта
};

static std::vector<Room> gRooms;
static uint16_t gNextId = 1;
static httpd_handle_t gServer = nullptr;

// ---------- помощники (вызываются под Lock) ----------
static Room* findRoom(uint16_t id) {
  for (auto& r : gRooms) if (r.id == id) return &r;
  return nullptr;
}

static std::string nameOf(const std::string& tok) {
  Session* s = sessionFind(tok);
  return s ? s->name : std::string("?");
}

static cJSON* membersJson(Room& r) {
  cJSON* m = cJSON_CreateArray();
  for (auto& t : r.members) cJSON_AddItemToArray(m, cJSON_CreateString(nameOf(t).c_str()));
  return m;
}

static cJSON* roomToJson(Room& r) {
  cJSON* o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", r.id);
  cJSON_AddStringToObject(o, "game", r.game.c_str());
  cJSON_AddStringToObject(o, "title", r.title.c_str());
  cJSON_AddNumberToObject(o, "max", r.maxP);
  cJSON_AddBoolToObject(o, "open", r.open);
  cJSON_AddStringToObject(o, "owner", nameOf(r.ownerTok).c_str());
  cJSON_AddNumberToObject(o, "count", (int)r.members.size());
  cJSON_AddItemToObject(o, "members", membersJson(r));
  return o;
}

static void wsSend(int fd, const std::string& payload) {
  if (!gServer || fd < 0) return;
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT;
  f.payload = (uint8_t*)payload.data();
  f.len = payload.size();
  f.final = true;
  esp_err_t e = httpd_ws_send_frame_async(gServer, fd, &f);
  if (e != ESP_OK) ESP_LOGD(TAG, "ws send fd %d: %s", fd, esp_err_to_name(e));
}

static void sendTo(const std::string& tok, const std::string& payload) {
  Session* s = sessionFind(tok);
  if (s && s->wsFd >= 0) wsSend(s->wsFd, payload);
}

static void broadcast(Room& r, const std::string& payload, const std::string* except = nullptr) {
  for (auto& t : r.members) {
    if (except && t == *except) continue;
    sendTo(t, payload);
  }
}

static void roomEvent(Room& r, const char* type, const std::string& who) {
  JDoc d(cJSON_CreateObject());
  cJSON_AddStringToObject(d.p, "t", type);
  cJSON_AddNumberToObject(d.p, "room", r.id);
  cJSON_AddStringToObject(d.p, "name", who.c_str());
  cJSON_AddStringToObject(d.p, "owner", nameOf(r.ownerTok).c_str());
  cJSON_AddItemToObject(d.p, "members", membersJson(r));
  broadcast(r, jdump(d.p));
}

static void leaveLocked(Session& s) {
  if (!s.room) return;
  uint16_t rid = s.room;
  s.room = 0;
  for (auto it = gRooms.begin(); it != gRooms.end(); ++it) {
    if (it->id != rid) continue;
    auto m = std::find(it->members.begin(), it->members.end(), s.token);
    if (m != it->members.end()) it->members.erase(m);
    if (it->members.empty()) {
      gRooms.erase(it);
    } else {
      if (it->ownerTok == s.token) it->ownerTok = it->members[0];
      roomEvent(*it, "leave", s.name);
    }
    return;
  }
}

static void joinLocked(Session& s, Room& r) {
  r.members.push_back(s.token);
  s.room = r.id;
  if (s.wsFd < 0) s.wsLost = millis();
  roomEvent(r, "join", s.name);
}

// ---------- публичное API ----------
bool lobbyCreate(const std::string& tok, const std::string& game, const std::string& title, int maxP, uint16_t& id, std::string& err) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (!s) { err = "Нет сессии"; return false; }
  int gm = gameMax(game);
  if (gm < 2) { err = "Игра не поддерживает мультиплеер"; return false; }
  if (gRooms.size() >= MAX_ROOMS) { err = "Слишком много комнат"; return false; }
  leaveLocked(*s);
  Room r;
  r.id = gNextId++;
  if (gNextId == 0) gNextId = 1;
  r.game = game;
  std::string t = title;
  size_t a = t.find_first_not_of(" \t\r\n");
  size_t b = t.find_last_not_of(" \t\r\n");
  t = (a == std::string::npos) ? "" : t.substr(a, b - a + 1);
  if (t.size() > 24) {
    t.resize(24);
    while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();   // не рвём UTF-8
    if (!t.empty() && static_cast<unsigned char>(t.back()) >= 0xC0) t.pop_back();
  }
  r.title = t.size() ? t : (s->name + "'s game");
  r.maxP = std::clamp(maxP > 0 ? maxP : gm, 2, gm);
  r.ownerTok = tok;
  gRooms.push_back(r);
  Room& ref = gRooms.back();
  joinLocked(*s, ref);
  id = ref.id;
  return true;
}

bool lobbyJoin(const std::string& tok, uint16_t id, std::string& err) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (!s) { err = "Нет сессии"; return false; }
  Room* r = findRoom(id);
  if (!r) { err = "Комната не найдена"; return false; }
  if (s->room == id) return true;
  if (!r->open) { err = "Игра уже началась"; return false; }
  if (r->members.size() >= r->maxP) { err = "Комната заполнена"; return false; }
  leaveLocked(*s);
  r = findRoom(id);   // указатель мог стать недействительным
  if (!r) { err = "Комната не найдена"; return false; }
  joinLocked(*s, *r);
  return true;
}

bool lobbyQuick(const std::string& tok, const std::string& game, uint16_t& id, std::string& err) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (!s) { err = "Нет сессии"; return false; }
  if (gameMax(game) < 2) { err = "Игра не поддерживает мультиплеер"; return false; }
  Room* best = nullptr;
  for (auto& r : gRooms) {
    if (r.game != game || !r.open || r.members.size() >= r.maxP) continue;
    if (s->room == r.id) continue;
    if (!best || r.members.size() > best->members.size()) best = &r;
  }
  if (best) {
    id = best->id;
    return lobbyJoin(tok, id, err);
  }
  return lobbyCreate(tok, game, "Быстрая игра", 0, id, err);
}

bool lobbyStart(const std::string& tok, std::string& err) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (!s || !s->room) { err = "Вы не в комнате"; return false; }
  Room* r = findRoom(s->room);
  if (!r) { err = "Комната не найдена"; return false; }
  if (r->ownerTok != tok) { err = "Старт может только хост"; return false; }
  if (r->members.size() < 2) { err = "Нужно минимум 2 игрока"; return false; }
  r->open = false;
  JDoc d(cJSON_CreateObject());
  cJSON_AddStringToObject(d.p, "t", "start");
  cJSON_AddNumberToObject(d.p, "room", r->id);
  cJSON_AddStringToObject(d.p, "game", r->game.c_str());
  broadcast(*r, jdump(d.p));
  return true;
}

void lobbyLeave(const std::string& tok) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (s) leaveLocked(*s);
}

std::string lobbyRoomsJson(const std::string& game) {
  Lock lk;
  JDoc a(cJSON_CreateArray());
  for (auto& r : gRooms) {
    if (!game.empty() && r.game != game) continue;
    cJSON_AddItemToArray(a.p, roomToJson(r));
  }
  return jdump(a.p);
}

std::string lobbyRoomJson(uint16_t id) {
  Lock lk;
  Room* r = findRoom(id);
  if (!id || !r) return "null";
  JDoc o(roomToJson(*r));
  return jdump(o.p);
}

std::string lobbyPlayersJson() {
  Lock lk;
  JDoc a(cJSON_CreateArray());
  for (auto& s : sessionsRaw()) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", s.name.c_str());
    cJSON_AddBoolToObject(o, "guest", s.guest);
    cJSON_AddNumberToObject(o, "room", s.room);
    cJSON_AddItemToArray(a.p, o);
  }
  return jdump(a.p);
}

void lobbyStats(int& players, int& rooms) {
  Lock lk;
  players = sessionsRaw().size();
  rooms = gRooms.size();
}

std::vector<std::string> lobbyPlayerLines() {
  Lock lk;
  std::vector<std::string> v;
  for (auto& s : sessionsRaw()) {
    std::string l = s.name;
    if (s.room) l += " #" + std::to_string(s.room);
    v.push_back(l);
  }
  return v;
}

std::vector<std::string> lobbyRoomLines() {
  Lock lk;
  std::vector<std::string> v;
  for (auto& r : gRooms) {
    v.push_back("#" + std::to_string(r.id) + " " + r.game + " " + std::to_string(r.members.size()) + "/" +
                std::to_string(r.maxP) + (r.open ? "" : " *"));
  }
  return v;
}

// периодическая уборка: протухшие сессии, игроки с потерянным WS
void lobbyTick() {
  Lock lk;
  uint32_t now = millis();
  auto& v = sessionsRaw();
  for (size_t i = 0; i < v.size();) {
    Session& s = v[i];
    if (s.room && s.wsFd < 0 && now - s.wsLost > WS_GRACE_MS) leaveLocked(s);
    if (s.wsFd < 0 && now - s.lastSeen > SESSION_IDLE_MS) {
      leaveLocked(s);
      v.erase(v.begin() + i);
    } else {
      i++;
    }
  }
}

void lobbyOnSocketClosed(int fd) {
  Lock lk;
  for (auto& s : sessionsRaw()) {
    if (s.wsFd == fd) {
      s.wsFd = -1;
      s.wsLost = millis();   // из комнаты не выходим сразу: идёт переход портал -> игра
    }
  }
}

// ---------- WebSocket ----------
static Session* sessionByWs(int fd) {
  for (auto& s : sessionsRaw()) if (s.wsFd == fd) return &s;
  return nullptr;
}

static void sendWelcome(int fd, Session& s) {
  JDoc d(cJSON_CreateObject());
  cJSON_AddStringToObject(d.p, "t", "welcome");
  cJSON_AddStringToObject(d.p, "name", s.name.c_str());
  cJSON_AddBoolToObject(d.p, "guest", s.guest);
  cJSON_AddNumberToObject(d.p, "room", s.room);
  Room* r = s.room ? findRoom(s.room) : nullptr;
  if (r) {
    cJSON_AddStringToObject(d.p, "game", r->game.c_str());
    cJSON_AddStringToObject(d.p, "owner", nameOf(r->ownerTok).c_str());
    cJSON_AddBoolToObject(d.p, "open", r->open);
    cJSON_AddItemToObject(d.p, "members", membersJson(*r));
  }
  wsSend(fd, jdump(d.p));
}

static void handleMessage(int fd, const std::string& text) {
  JDoc doc(cJSON_Parse(text.c_str()));
  if (!doc.p) return;
  std::string t = jstr(doc.p, "t");

  Lock lk;
  if (t == "hello") {
    std::string tok = jstr(doc.p, "token");
    Session* s = sessionFind(tok);
    if (!s) {
      wsSend(fd, "{\"t\":\"error\",\"e\":\"auth\"}");
      httpd_sess_trigger_close(gServer, fd);
      return;
    }
    if (s->wsFd >= 0 && s->wsFd != fd) httpd_sess_trigger_close(gServer, s->wsFd);
    s->wsFd = fd;
    s->wsLost = 0;
    s->lastSeen = millis();
    sendWelcome(fd, *s);
    return;
  }

  Session* s = sessionByWs(fd);
  if (!s) return;
  s->lastSeen = millis();

  if (t == "ping") {
    wsSend(fd, "{\"t\":\"pong\"}");
  } else if (t == "msg") {
    if (!s->room) return;
    Room* r = findRoom(s->room);
    if (!r) return;
    JDoc out(cJSON_CreateObject());
    cJSON_AddStringToObject(out.p, "t", "msg");
    cJSON_AddStringToObject(out.p, "from", s->name.c_str());
    cJSON* data = cJSON_DetachItemFromObjectCaseSensitive(doc.p, "d");
    cJSON_AddItemToObject(out.p, "d", data ? data : cJSON_CreateNull());
    std::string tok = s->token;
    broadcast(*r, jdump(out.p), &tok);
  } else if (t == "start") {
    std::string err;
    if (!lobbyStart(s->token, err)) {
      JDoc e(cJSON_CreateObject());
      cJSON_AddStringToObject(e.p, "t", "error");
      cJSON_AddStringToObject(e.p, "e", err.c_str());
      wsSend(fd, jdump(e.p));
    }
  } else if (t == "leave") {
    leaveLocked(*s);
    wsSend(fd, "{\"t\":\"left\"}");
  }
}

static esp_err_t wsHandler(httpd_req_t* req) {
  if (req->method == HTTP_GET) return ESP_OK;   // рукопожатие уже выполнено сервером
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
  if (e != ESP_OK) return e;
  if (f.type != HTTPD_WS_TYPE_TEXT || f.len == 0 || f.len > 1024) {
    if (f.len > 0 && f.len <= 1024) {   // вычитать и выбросить, чтобы не рассинхронизировать поток
      std::string junk(f.len, '\0');
      f.payload = (uint8_t*)&junk[0];
      httpd_ws_recv_frame(req, &f, f.len);
    } else if (f.len > 1024) {
      return ESP_FAIL;
    }
    return ESP_OK;
  }
  std::string buf(f.len, '\0');
  f.payload = (uint8_t*)&buf[0];
  e = httpd_ws_recv_frame(req, &f, f.len);
  if (e != ESP_OK) return e;
  handleMessage(httpd_req_to_sockfd(req), buf);
  return ESP_OK;
}

void lobbyBegin(httpd_handle_t server) {
  gServer = server;
  httpd_uri_t u = {};
  u.uri = "/ws";
  u.method = HTTP_GET;
  u.handler = wsHandler;
  u.is_websocket = true;
  httpd_register_uri_handler(server, &u);
}
