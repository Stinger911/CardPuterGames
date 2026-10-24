#include "lobby.h"
#include "auth.h"
#include "config.h"
#include "storage.h"
#include <ArduinoJson.h>
#include <algorithm>

struct Room {
  uint16_t id;
  String game;
  String title;
  uint8_t maxP;
  String ownerTok;
  std::vector<String> members;   // токены
  bool open = true;              // false после старта
};

static std::vector<Room> gRooms;
static uint16_t gNextId = 1;
static AsyncWebSocket ws("/ws");

// ---------- помощники (вызываются под Lock) ----------
static Room* findRoom(uint16_t id) {
  for (auto& r : gRooms) if (r.id == id) return &r;
  return nullptr;
}

static String nameOf(const String& tok) {
  Session* s = sessionFind(tok);
  return s ? s->name : String("?");
}

static void roomToJson(Room& r, JsonObject o) {
  o["id"] = r.id;
  o["game"] = r.game;
  o["title"] = r.title;
  o["max"] = r.maxP;
  o["open"] = r.open;
  o["owner"] = nameOf(r.ownerTok);
  o["count"] = (int)r.members.size();
  JsonArray m = o["members"].to<JsonArray>();
  for (auto& t : r.members) m.add(nameOf(t));
}

static void sendTo(const String& tok, const String& payload) {
  Session* s = sessionFind(tok);
  if (s && s->wsId) ws.text(s->wsId, payload);
}

static void broadcast(Room& r, const String& payload, const String* except = nullptr) {
  for (auto& t : r.members) {
    if (except && t == *except) continue;
    sendTo(t, payload);
  }
}

static void roomEvent(Room& r, const char* type, const String& who) {
  JsonDocument d;
  d["t"] = type;
  d["room"] = r.id;
  d["name"] = who;
  d["owner"] = nameOf(r.ownerTok);
  JsonArray m = d["members"].to<JsonArray>();
  for (auto& t : r.members) m.add(nameOf(t));
  String s;
  serializeJson(d, s);
  broadcast(r, s);
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
  if (!s.wsId) s.wsLost = millis();
  roomEvent(r, "join", s.name);
}

// ---------- публичное API ----------
bool lobbyCreate(const String& tok, const String& game, const String& title, int maxP, uint16_t& id, String& err) {
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
  String t = title;
  t.trim();
  if (t.length() > 24) t = t.substring(0, 24);
  r.title = t.length() ? t : (s->name + "'s game");
  r.maxP = constrain(maxP > 0 ? maxP : gm, 2, gm);
  r.ownerTok = tok;
  gRooms.push_back(r);
  Room& ref = gRooms.back();
  joinLocked(*s, ref);
  id = ref.id;
  return true;
}

bool lobbyJoin(const String& tok, uint16_t id, String& err) {
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

bool lobbyQuick(const String& tok, const String& game, uint16_t& id, String& err) {
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

bool lobbyStart(const String& tok, String& err) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (!s || !s->room) { err = "Вы не в комнате"; return false; }
  Room* r = findRoom(s->room);
  if (!r) { err = "Комната не найдена"; return false; }
  if (r->ownerTok != tok) { err = "Старт может только хост"; return false; }
  if (r->members.size() < 2) { err = "Нужно минимум 2 игрока"; return false; }
  r->open = false;
  JsonDocument d;
  d["t"] = "start";
  d["room"] = r->id;
  d["game"] = r->game;
  String out;
  serializeJson(d, out);
  broadcast(*r, out);
  return true;
}

void lobbyLeave(const String& tok) {
  Lock lk;
  Session* s = sessionFind(tok);
  if (s) leaveLocked(*s);
}

String lobbyRoomsJson(const String& game) {
  Lock lk;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto& r : gRooms) {
    if (game.length() && r.game != game) continue;
    roomToJson(r, a.add<JsonObject>());
  }
  String s;
  serializeJson(d, s);
  return s;
}

String lobbyRoomJson(uint16_t id) {
  Lock lk;
  Room* r = findRoom(id);
  if (!id || !r) return "null";
  JsonDocument d;
  roomToJson(*r, d.to<JsonObject>());
  String s;
  serializeJson(d, s);
  return s;
}

String lobbyPlayersJson() {
  Lock lk;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto& s : sessionsRaw()) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = s.name;
    o["guest"] = s.guest;
    o["room"] = s.room;
  }
  String out;
  serializeJson(d, out);
  return out;
}

void lobbyStats(int& players, int& rooms) {
  Lock lk;
  players = sessionsRaw().size();
  rooms = gRooms.size();
}

std::vector<String> lobbyPlayerLines() {
  Lock lk;
  std::vector<String> v;
  for (auto& s : sessionsRaw()) {
    String l = s.name;
    if (s.room) l += " #" + String(s.room);
    v.push_back(l);
  }
  return v;
}

std::vector<String> lobbyRoomLines() {
  Lock lk;
  std::vector<String> v;
  for (auto& r : gRooms) {
    v.push_back("#" + String(r.id) + " " + r.game + " " + String((int)r.members.size()) + "/" + String(r.maxP) + (r.open ? "" : " *"));
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
    if (s.room && !s.wsId && now - s.wsLost > WS_GRACE_MS) leaveLocked(s);
    if (!s.wsId && now - s.lastSeen > SESSION_IDLE_MS) {
      leaveLocked(s);
      v.erase(v.begin() + i);
    } else {
      i++;
    }
  }
}

void lobbyCleanupWs() { ws.cleanupClients(); }

// ---------- WebSocket ----------
static Session* sessionByWs(uint32_t id) {
  for (auto& s : sessionsRaw()) if (s.wsId == id) return &s;
  return nullptr;
}

static void sendWelcome(AsyncWebSocketClient* c, Session& s) {
  JsonDocument d;
  d["t"] = "welcome";
  d["name"] = s.name;
  d["guest"] = s.guest;
  d["room"] = s.room;
  Room* r = s.room ? findRoom(s.room) : nullptr;
  if (r) {
    d["game"] = r->game;
    d["owner"] = nameOf(r->ownerTok);
    d["open"] = r->open;
    JsonArray m = d["members"].to<JsonArray>();
    for (auto& t : r->members) m.add(nameOf(t));
  }
  String out;
  serializeJson(d, out);
  c->text(out);
}

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_DISCONNECT) {
    Lock lk;
    Session* s = sessionByWs(c->id());
    if (s) {
      s->wsId = 0;
      s->wsLost = millis();   // из комнаты не выходим сразу: идёт переход портал -> игра
    }
    return;
  }
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) return;
  if (len > 1024) return;

  JsonDocument doc;
  if (deserializeJson(doc, (const char*)data, len)) return;
  String t = doc["t"] | "";

  Lock lk;
  if (t == "hello") {
    String tok = doc["token"] | "";
    Session* s = sessionFind(tok);
    if (!s) {
      c->text("{\"t\":\"error\",\"e\":\"auth\"}");
      c->close();
      return;
    }
    if (s->wsId && s->wsId != c->id()) ws.close(s->wsId);
    s->wsId = c->id();
    s->wsLost = 0;
    s->lastSeen = millis();
    sendWelcome(c, *s);
    return;
  }

  Session* s = sessionByWs(c->id());
  if (!s) return;
  s->lastSeen = millis();

  if (t == "ping") {
    c->text("{\"t\":\"pong\"}");
  } else if (t == "msg") {
    if (!s->room) return;
    Room* r = findRoom(s->room);
    if (!r) return;
    JsonDocument out;
    out["t"] = "msg";
    out["from"] = s->name;
    out["d"] = doc["d"];
    String str;
    serializeJson(out, str);
    broadcast(*r, str, &s->token);
  } else if (t == "start") {
    String err;
    if (!lobbyStart(s->token, err)) {
      JsonDocument e;
      e["t"] = "error";
      e["e"] = err;
      String str;
      serializeJson(e, str);
      c->text(str);
    }
  } else if (t == "leave") {
    leaveLocked(*s);
    c->text("{\"t\":\"left\"}");
  }
}

void lobbyBegin(AsyncWebServer& server) {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
}
