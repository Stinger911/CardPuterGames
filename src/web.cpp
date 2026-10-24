#include "web.h"
#include "config.h"
#include "storage.h"
#include "auth.h"
#include "lobby.h"
#include "portal_html.h"
#include "sdk_js.h"
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <functional>

static AsyncWebServer server(80);

// ---------- помощники ----------
static void sendJson(AsyncWebServerRequest* r, int code, const String& body, const String& cookie = "") {
  AsyncWebServerResponse* resp = r->beginResponse(code, "application/json", body);
  resp->addHeader("Cache-Control", "no-store");
  if (cookie.length()) resp->addHeader("Set-Cookie", "sid=" + cookie + "; Path=/; Max-Age=86400");
  r->send(resp);
}

static void sendErr(AsyncWebServerRequest* r, int code, const String& msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  String s;
  serializeJson(d, s);
  sendJson(r, code, s);
}

static void sendToken(AsyncWebServerRequest* r, const String& token, const String& name) {
  JsonDocument d;
  d["ok"] = true;
  d["token"] = token;
  d["name"] = name;
  String s;
  serializeJson(d, s);
  sendJson(r, 200, s, token);
}

static bool getToken(AsyncWebServerRequest* r, String& tok) {
  if (r->hasHeader("X-Token")) {
    tok = r->header("X-Token");
    if (tok.length()) return true;
  }
  if (r->hasHeader("Cookie")) {
    String c = r->header("Cookie");
    int i = c.indexOf("sid=");
    if (i >= 0) {
      int e = c.indexOf(';', i);
      tok = c.substring(i + 4, e < 0 ? c.length() : e);
      if (tok.length()) return true;
    }
  }
  if (r->hasParam("t")) {
    tok = r->getParam("t")->value();
    return tok.length() > 0;
  }
  return false;
}

static bool authed(AsyncWebServerRequest* r, Session& s) {
  String t;
  if (!getToken(r, t) || !sessionGet(t, s)) {
    sendErr(r, 401, "Требуется вход");
    return false;
  }
  return true;
}

// ответ {"ok":true,"room":{...}}
static void sendRoom(AsyncWebServerRequest* r, uint16_t id) {
  String room = lobbyRoomJson(id);
  JsonDocument d;
  d["ok"] = true;
  d["room"] = serialized(room);
  String s;
  serializeJson(d, s);
  sendJson(r, 200, s);
}

typedef std::function<void(AsyncWebServerRequest*, JsonObject)> JsonFn;
static void addJson(const char* uri, JsonFn fn) {
  auto* h = new AsyncCallbackJsonWebHandler(uri, [fn](AsyncWebServerRequest* r, JsonVariant& j) {
    fn(r, j.as<JsonObject>());
  });
  h->setMethod(HTTP_POST);
  h->setMaxContentLength(1024);
  server.addHandler(h);
}

// ---------- маршруты ----------
void webBegin() {
  // --- страница портала и SDK ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (sdOk() && SD.exists("/www/index.html")) {
      r->send(SD, "/www/index.html", "text/html");
    } else {
      r->send_P(200, "text/html", (const uint8_t*)PORTAL_HTML, strlen(PORTAL_HTML));
    }
  });
  server.on("/sdk.js", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp = r->beginResponse_P(200, "application/javascript", (const uint8_t*)SDK_JS, strlen(SDK_JS));
    resp->addHeader("Cache-Control", "max-age=3600");
    r->send(resp);
  });
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(204); });

  // --- игры и ассеты с SD ---
  if (sdOk()) {
    server.serveStatic("/g/", SD, "/games/").setDefaultFile("index.html").setCacheControl("max-age=300");
    server.serveStatic("/assets/", SD, "/www/assets/").setCacheControl("max-age=300");
  }

  // --- вход / регистрация ---
  addJson("/api/register", [](AsyncWebServerRequest* r, JsonObject o) {
    String name = o["name"] | "";
    String pass = o["pass"] | "";
    String err;
    if (!authRegister(name, pass, err)) { sendErr(r, 400, err); return; }
    String tok = sessionCreate(name, false);
    if (!tok.length()) { sendErr(r, 503, "Сервер заполнен"); return; }
    sendToken(r, tok, name);
  });

  addJson("/api/login", [](AsyncWebServerRequest* r, JsonObject o) {
    String name = o["name"] | "";
    String pass = o["pass"] | "";
    String canon, err;
    if (!authLogin(name, pass, canon, err)) { sendErr(r, 401, err); return; }
    String tok = sessionCreate(canon, false);
    if (!tok.length()) { sendErr(r, 503, "Сервер заполнен"); return; }
    sendToken(r, tok, canon);
  });

  addJson("/api/guest", [](AsyncWebServerRequest* r, JsonObject) {
    if (!cfg.allowGuests) { sendErr(r, 403, "Гостевой режим отключён"); return; }
    String name = guestName();
    String tok = sessionCreate(name, true);
    if (!tok.length()) { sendErr(r, 503, "Сервер заполнен"); return; }
    sendToken(r, tok, name);
  });

  addJson("/api/logout", [](AsyncWebServerRequest* r, JsonObject) {
    String t;
    if (getToken(r, t)) {
      lobbyLeave(t);
      sessionRemove(t);
    }
    sendJson(r, 200, "{\"ok\":true}");
  });

  // --- информация ---
  server.on("/api/me", HTTP_GET, [](AsyncWebServerRequest* r) {
    Session s;
    if (!authed(r, s)) return;
    JsonDocument d;
    d["ok"] = true;
    d["name"] = s.name;
    d["guest"] = s.guest;
    d["room"] = serialized(lobbyRoomJson(s.room));
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  server.on("/api/games", HTTP_GET, [](AsyncWebServerRequest* r) {
    Session s;
    if (!authed(r, s)) return;
    JsonDocument d;
    d["ok"] = true;
    d["sd"] = sdOk();
    d["games"] = serialized(gamesJson());
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  server.on("/api/players", HTTP_GET, [](AsyncWebServerRequest* r) {
    Session s;
    if (!authed(r, s)) return;
    JsonDocument d;
    d["ok"] = true;
    d["players"] = serialized(lobbyPlayersJson());
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  server.on("/api/rooms", HTTP_GET, [](AsyncWebServerRequest* r) {
    Session s;
    if (!authed(r, s)) return;
    String game = r->hasParam("game") ? r->getParam("game")->value() : "";
    if (game.length() && !validId(game)) { sendErr(r, 400, "Неверный id игры"); return; }
    JsonDocument d;
    d["ok"] = true;
    d["rooms"] = serialized(lobbyRoomsJson(game));
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    int p, rm;
    lobbyStats(p, rm);
    JsonDocument d;
    d["ok"] = true;
    d["name"] = FW_NAME;
    d["version"] = FW_VERSION;
    d["uptime"] = (uint32_t)(millis() / 1000);
    d["heap"] = ESP.getFreeHeap();
    d["sd"] = sdOk();
    d["players"] = p;
    d["rooms"] = rm;
    String out;
    serializeJson(d, out);
    sendJson(r, 200, out);
  });

  // --- лобби ---
  addJson("/api/room/create", [](AsyncWebServerRequest* r, JsonObject o) {
    Session s;
    if (!authed(r, s)) return;
    String game = o["game"] | "";
    String title = o["title"] | "";
    int mx = o["max"] | 0;
    uint16_t id;
    String err;
    if (!lobbyCreate(s.token, game, title, mx, id, err)) { sendErr(r, 400, err); return; }
    sendRoom(r, id);
  });

  addJson("/api/room/join", [](AsyncWebServerRequest* r, JsonObject o) {
    Session s;
    if (!authed(r, s)) return;
    uint16_t id = o["id"] | 0;
    String err;
    if (!lobbyJoin(s.token, id, err)) { sendErr(r, 400, err); return; }
    sendRoom(r, id);
  });

  addJson("/api/room/quick", [](AsyncWebServerRequest* r, JsonObject o) {
    Session s;
    if (!authed(r, s)) return;
    String game = o["game"] | "";
    uint16_t id;
    String err;
    if (!lobbyQuick(s.token, game, id, err)) { sendErr(r, 400, err); return; }
    sendRoom(r, id);
  });

  addJson("/api/room/start", [](AsyncWebServerRequest* r, JsonObject) {
    Session s;
    if (!authed(r, s)) return;
    String err;
    if (!lobbyStart(s.token, err)) { sendErr(r, 400, err); return; }
    sendJson(r, 200, "{\"ok\":true}");
  });

  addJson("/api/room/leave", [](AsyncWebServerRequest* r, JsonObject) {
    Session s;
    if (!authed(r, s)) return;
    lobbyLeave(s.token);
    sendJson(r, 200, "{\"ok\":true}");
  });

  lobbyBegin(server);

  // --- captive portal: всё неизвестное уводим на главную ---
  server.onNotFound([](AsyncWebServerRequest* r) {
    String u = r->url();
    if (u.startsWith("/api/")) { sendErr(r, 404, "Не найдено"); return; }
    if (u.startsWith("/g/") || u.startsWith("/assets/")) { r->send(404, "text/plain", "Not found"); return; }
    r->redirect("http://" AP_IP_STR "/");
  });

  server.begin();
}
