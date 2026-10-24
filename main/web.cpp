#include "web.hpp"

#include <cstring>
#include <memory>

#include <unistd.h>

#include "auth.hpp"
#include "config.hpp"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "json_util.hpp"
#include "lobby.hpp"
#include "portal_html.hpp"
#include "sdk_js.hpp"
#include "storage.hpp"

static const char* TAG = "web";
static httpd_handle_t server = nullptr;

// ---------- помощники ----------
static const char* statusText(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 413: return "413 Payload Too Large";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
  }
}

static esp_err_t sendJson(httpd_req_t* r, int code, const std::string& body, const std::string& cookie = "") {
  httpd_resp_set_status(r, statusText(code));
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  std::string ck;
  if (!cookie.empty()) {
    ck = "sid=" + cookie + "; Path=/; Max-Age=86400";
    httpd_resp_set_hdr(r, "Set-Cookie", ck.c_str());
  }
  return httpd_resp_send(r, body.data(), body.size());
}

static esp_err_t sendErr(httpd_req_t* r, int code, const std::string& msg) {
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", false);
  cJSON_AddStringToObject(d.p, "error", msg.c_str());
  return sendJson(r, code, jdump(d.p));
}

static esp_err_t sendToken(httpd_req_t* r, const std::string& token, const std::string& name) {
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", true);
  cJSON_AddStringToObject(d.p, "token", token.c_str());
  cJSON_AddStringToObject(d.p, "name", name.c_str());
  return sendJson(r, 200, jdump(d.p), token);
}

static std::string getHeader(httpd_req_t* r, const char* name) {
  size_t n = httpd_req_get_hdr_value_len(r, name);
  if (!n) return "";
  std::string v(n + 1, '\0');
  if (httpd_req_get_hdr_value_str(r, name, &v[0], n + 1) != ESP_OK) return "";
  v.resize(n);
  return v;
}

static std::string queryParam(httpd_req_t* r, const char* key) {
  size_t n = httpd_req_get_url_query_len(r);
  if (!n) return "";
  std::string q(n + 1, '\0');
  if (httpd_req_get_url_query_str(r, &q[0], n + 1) != ESP_OK) return "";
  char val[96];
  if (httpd_query_key_value(q.c_str(), key, val, sizeof val) != ESP_OK) return "";
  return val;
}

static bool getToken(httpd_req_t* r, std::string& tok) {
  tok = getHeader(r, "X-Token");
  if (!tok.empty()) return true;
  std::string c = getHeader(r, "Cookie");
  size_t i = c.find("sid=");
  if (i != std::string::npos) {
    size_t e = c.find(';', i);
    tok = c.substr(i + 4, e == std::string::npos ? std::string::npos : e - i - 4);
    if (!tok.empty()) return true;
  }
  tok = queryParam(r, "t");
  return !tok.empty();
}

static bool authed(httpd_req_t* r, Session& s) {
  std::string t;
  if (!getToken(r, t) || !sessionGet(t, s)) {
    sendErr(r, 401, "Требуется вход");
    return false;
  }
  return true;
}

// ответ {"ok":true,"room":{...}}
static esp_err_t sendRoom(httpd_req_t* r, uint16_t id) {
  return sendJson(r, 200, "{\"ok\":true,\"room\":" + lobbyRoomJson(id) + "}");
}

// ---------- маршруты API ----------
typedef esp_err_t (*JsonFn)(httpd_req_t*, cJSON*);

// Читает тело (до 1 КБ), разбирает JSON и вызывает обработчик из user_ctx.
static esp_err_t jsonPost(httpd_req_t* r) {
  if (r->content_len > 1024) return sendErr(r, 413, "Слишком большой запрос");
  std::string body(r->content_len, '\0');
  size_t got = 0;
  while (got < r->content_len) {
    int n = httpd_req_recv(r, &body[got], r->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;
    got += n;
  }
  JDoc j(cJSON_Parse(body.c_str()));
  if (!j.p) j.p = cJSON_CreateObject();
  return reinterpret_cast<JsonFn>(r->user_ctx)(r, j.p);
}

static void addPost(const char* uri, JsonFn fn) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = HTTP_POST;
  u.handler = jsonPost;
  u.user_ctx = reinterpret_cast<void*>(fn);
  httpd_register_uri_handler(server, &u);
}

static void addGet(const char* uri, esp_err_t (*fn)(httpd_req_t*)) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = HTTP_GET;
  u.handler = fn;
  httpd_register_uri_handler(server, &u);
}

// ---------- статические файлы с SD ----------
static const char* mimeOf(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
  for (auto& c : e) c = tolower((unsigned char)c);
  static const struct { const char* ext; const char* mime; } T[] = {
      {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
      {"js", "application/javascript"},     {"mjs", "application/javascript"},
      {"css", "text/css"},                  {"json", "application/json"},
      {"png", "image/png"},                 {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},               {"gif", "image/gif"},
      {"svg", "image/svg+xml"},             {"webp", "image/webp"},
      {"ico", "image/x-icon"},              {"txt", "text/plain; charset=utf-8"},
      {"mp3", "audio/mpeg"},                {"ogg", "audio/ogg"},
      {"wav", "audio/wav"},                 {"woff", "font/woff"},
      {"woff2", "font/woff2"},              {"wasm", "application/wasm"},
  };
  for (auto& t : T) if (e == t.ext) return t.mime;
  return "application/octet-stream";
}

static std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

// Отправляет файл (или его .gz-версию) чанками.
static esp_err_t sendFile(httpd_req_t* r, const std::string& path, const char* cache) {
  std::string real = path;
  bool gz = false;
  FILE* f = fopen(real.c_str(), "rb");
  if (!f) {
    real = path + ".gz";
    f = fopen(real.c_str(), "rb");
    gz = f != nullptr;
  }
  if (!f) { httpd_resp_send_404(r); return ESP_OK; }
  httpd_resp_set_type(r, mimeOf(path));
  if (gz) httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  if (cache) httpd_resp_set_hdr(r, "Cache-Control", cache);
  const size_t CH = 4096;
  std::unique_ptr<char[]> buf(new char[CH]);
  esp_err_t e = ESP_OK;
  size_t n;
  while ((n = fread(buf.get(), 1, CH, f)) > 0) {
    e = httpd_resp_send_chunk(r, buf.get(), n);
    if (e != ESP_OK) break;
  }
  fclose(f);
  if (e == ESP_OK) httpd_resp_send_chunk(r, nullptr, 0);
  return e;
}

// /g/<id>/... -> SD_ROOT/games/<id>/... ; /assets/... -> SD_ROOT/www/assets/...
static esp_err_t staticHandler(httpd_req_t* r) {
  std::string uri = r->uri;
  size_t q = uri.find('?');
  if (q != std::string::npos) uri.resize(q);
  const char* prefix = strncmp(uri.c_str(), "/g/", 3) == 0 ? "/g/" : "/assets/";
  std::string base = prefix[1] == 'g' ? SD_ROOT "/games/" : SD_ROOT "/www/assets/";
  std::string rel = urlDecode(uri.substr(strlen(prefix)));
  if (!sdOk() || rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos) {
    httpd_resp_send_404(r);
    return ESP_OK;
  }
  std::string path = base + rel;
  if (!rel.empty() && rel.back() != '/' && isDir(path)) {   // /g/duel -> /g/duel/ (чтобы относительные пути работали)
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", (uri + "/").c_str());
    return httpd_resp_send(r, nullptr, 0);
  }
  if (rel.empty() || rel.back() == '/') path += "index.html";
  return sendFile(r, path, "max-age=300");
}

// ---------- обработчики ----------
static esp_err_t hIndex(httpd_req_t* r) {
  if (sdOk() && fileExists(SD_ROOT "/www/index.html")) return sendFile(r, SD_ROOT "/www/index.html", nullptr);
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  return httpd_resp_send(r, PORTAL_HTML, strlen(PORTAL_HTML));
}

static esp_err_t hSdk(httpd_req_t* r) {
  httpd_resp_set_type(r, "application/javascript");
  httpd_resp_set_hdr(r, "Cache-Control", "max-age=3600");
  return httpd_resp_send(r, SDK_JS, strlen(SDK_JS));
}

static esp_err_t hFavicon(httpd_req_t* r) {
  httpd_resp_set_status(r, "204 No Content");
  return httpd_resp_send(r, nullptr, 0);
}

static esp_err_t hRegister(httpd_req_t* r, cJSON* o) {
  std::string name = jstr(o, "name"), pass = jstr(o, "pass"), err;
  if (!authRegister(name, pass, err)) return sendErr(r, 400, err);
  std::string tok = sessionCreate(name, false);
  if (tok.empty()) return sendErr(r, 503, "Сервер заполнен");
  return sendToken(r, tok, name);
}

static esp_err_t hLogin(httpd_req_t* r, cJSON* o) {
  std::string name = jstr(o, "name"), pass = jstr(o, "pass"), canon, err;
  if (!authLogin(name, pass, canon, err)) return sendErr(r, 401, err);
  std::string tok = sessionCreate(canon, false);
  if (tok.empty()) return sendErr(r, 503, "Сервер заполнен");
  return sendToken(r, tok, canon);
}

static esp_err_t hGuest(httpd_req_t* r, cJSON*) {
  if (!cfg.allowGuests) return sendErr(r, 403, "Гостевой режим отключён");
  std::string name = guestName();
  std::string tok = sessionCreate(name, true);
  if (tok.empty()) return sendErr(r, 503, "Сервер заполнен");
  return sendToken(r, tok, name);
}

static esp_err_t hLogout(httpd_req_t* r, cJSON*) {
  std::string t;
  if (getToken(r, t)) {
    lobbyLeave(t);
    sessionRemove(t);
  }
  return sendJson(r, 200, "{\"ok\":true}");
}

static esp_err_t hMe(httpd_req_t* r) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", true);
  cJSON_AddStringToObject(d.p, "name", s.name.c_str());
  cJSON_AddBoolToObject(d.p, "guest", s.guest);
  cJSON_AddRawToObject(d.p, "room", lobbyRoomJson(s.room).c_str());
  return sendJson(r, 200, jdump(d.p));
}

static esp_err_t hGames(httpd_req_t* r) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", true);
  cJSON_AddBoolToObject(d.p, "sd", sdOk());
  cJSON_AddRawToObject(d.p, "games", gamesJson().c_str());
  return sendJson(r, 200, jdump(d.p));
}

static esp_err_t hPlayers(httpd_req_t* r) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", true);
  cJSON_AddRawToObject(d.p, "players", lobbyPlayersJson().c_str());
  return sendJson(r, 200, jdump(d.p));
}

static esp_err_t hRooms(httpd_req_t* r) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  std::string game = queryParam(r, "game");
  if (!game.empty() && !validId(game)) return sendErr(r, 400, "Неверный id игры");
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", true);
  cJSON_AddRawToObject(d.p, "rooms", lobbyRoomsJson(game).c_str());
  return sendJson(r, 200, jdump(d.p));
}

static esp_err_t hStatus(httpd_req_t* r) {
  int p, rm;
  lobbyStats(p, rm);
  JDoc d(cJSON_CreateObject());
  cJSON_AddBoolToObject(d.p, "ok", true);
  cJSON_AddStringToObject(d.p, "name", FW_NAME);
  cJSON_AddStringToObject(d.p, "version", FW_VERSION);
  cJSON_AddNumberToObject(d.p, "uptime", millis() / 1000);
  cJSON_AddNumberToObject(d.p, "heap", esp_get_free_heap_size());
  cJSON_AddBoolToObject(d.p, "sd", sdOk());
  cJSON_AddNumberToObject(d.p, "players", p);
  cJSON_AddNumberToObject(d.p, "rooms", rm);
  return sendJson(r, 200, jdump(d.p));
}

static esp_err_t hRoomCreate(httpd_req_t* r, cJSON* o) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  uint16_t id;
  std::string err;
  if (!lobbyCreate(s.token, jstr(o, "game"), jstr(o, "title"), jint(o, "max", 0), id, err)) return sendErr(r, 400, err);
  return sendRoom(r, id);
}

static esp_err_t hRoomJoin(httpd_req_t* r, cJSON* o) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  uint16_t id = jint(o, "id", 0);
  std::string err;
  if (!lobbyJoin(s.token, id, err)) return sendErr(r, 400, err);
  return sendRoom(r, id);
}

static esp_err_t hRoomQuick(httpd_req_t* r, cJSON* o) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  uint16_t id;
  std::string err;
  if (!lobbyQuick(s.token, jstr(o, "game"), id, err)) return sendErr(r, 400, err);
  return sendRoom(r, id);
}

static esp_err_t hRoomStart(httpd_req_t* r, cJSON*) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  std::string err;
  if (!lobbyStart(s.token, err)) return sendErr(r, 400, err);
  return sendJson(r, 200, "{\"ok\":true}");
}

static esp_err_t hRoomLeave(httpd_req_t* r, cJSON*) {
  Session s;
  if (!authed(r, s)) return ESP_OK;
  lobbyLeave(s.token);
  return sendJson(r, 200, "{\"ok\":true}");
}

// captive portal: всё неизвестное уводим на главную
static esp_err_t notFound(httpd_req_t* r, httpd_err_code_t) {
  std::string u = r->uri;
  if (u.rfind("/api/", 0) == 0) { sendErr(r, 404, "Не найдено"); return ESP_OK; }
  if (u.rfind("/g/", 0) == 0 || u.rfind("/assets/", 0) == 0) {
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_send(r, "Not found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  httpd_resp_set_status(r, "302 Found");
  httpd_resp_set_hdr(r, "Location", "http://" AP_IP_STR "/");
  httpd_resp_send(r, nullptr, 0);
  return ESP_OK;
}

static void onClose(httpd_handle_t, int fd) {
  lobbyOnSocketClosed(fd);
  close(fd);
}

void webBegin() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.max_uri_handlers = 32;
  c.max_open_sockets = 12;
  c.stack_size = 10240;
  c.lru_purge_enable = true;
  c.uri_match_fn = httpd_uri_match_wildcard;
  c.close_fn = onClose;
  if (httpd_start(&server, &c) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }

  addGet("/", hIndex);
  addGet("/sdk.js", hSdk);
  addGet("/favicon.ico", hFavicon);
  addGet("/g/*", staticHandler);
  addGet("/assets/*", staticHandler);

  addPost("/api/register", hRegister);
  addPost("/api/login", hLogin);
  addPost("/api/guest", hGuest);
  addPost("/api/logout", hLogout);
  addGet("/api/me", hMe);
  addGet("/api/games", hGames);
  addGet("/api/players", hPlayers);
  addGet("/api/rooms", hRooms);
  addGet("/api/status", hStatus);
  addPost("/api/room/create", hRoomCreate);
  addPost("/api/room/join", hRoomJoin);
  addPost("/api/room/quick", hRoomQuick);
  addPost("/api/room/start", hRoomStart);
  addPost("/api/room/leave", hRoomLeave);

  lobbyBegin(server);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, notFound);
}
