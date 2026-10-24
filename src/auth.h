#pragma once
#include <Arduino.h>
#include <vector>

struct Session {
  String   token;
  String   name;
  bool     guest   = false;
  uint32_t lastSeen = 0;
  uint16_t room    = 0;    // 0 = не в комнате
  uint32_t wsId    = 0;    // 0 = WebSocket не подключён
  uint32_t wsLost  = 0;    // millis() когда потеряли WS / вошли в комнату без WS
};

// RAII-блокировка общего состояния (сессии + комнаты). Рекурсивная.
class Lock {
 public:
  Lock();
  ~Lock();
};

void authBegin();

bool authRegister(const String& name, const String& pass, String& err);
bool authLogin(const String& name, const String& pass, String& canonicalName, String& err);

String sessionCreate(const String& name, bool guest);          // "" если мест нет
bool   sessionGet(const String& token, Session& out, bool touch = true);
void   sessionRemove(const String& token);                     // только удаляет (комнату чистит lobbyLeave)
String guestName();

// Только под Lock:
std::vector<Session>& sessionsRaw();
Session* sessionFind(const String& token);
