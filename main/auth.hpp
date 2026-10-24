#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Session {
  std::string token;
  std::string name;
  bool        guest    = false;
  uint32_t    lastSeen = 0;
  uint16_t    room     = 0;    // 0 = не в комнате
  int         wsFd     = -1;   // -1 = WebSocket не подключён
  uint32_t    wsLost   = 0;    // millis() когда потеряли WS / вошли в комнату без WS
};

// RAII-блокировка общего состояния (сессии + комнаты). Рекурсивная.
class Lock {
 public:
  Lock();
  ~Lock();
};

void authBegin();

bool authRegister(const std::string& name, const std::string& pass, std::string& err);
bool authLogin(const std::string& name, const std::string& pass, std::string& canonicalName, std::string& err);

std::string sessionCreate(const std::string& name, bool guest);          // "" если мест нет
bool        sessionGet(const std::string& token, Session& out, bool touch = true);
void        sessionRemove(const std::string& token);                     // только удаляет (комнату чистит lobbyLeave)
std::string guestName();

// Только под Lock:
std::vector<Session>& sessionsRaw();
Session* sessionFind(const std::string& token);
