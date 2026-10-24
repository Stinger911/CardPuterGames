#pragma once
#include <Arduino.h>
#include <vector>
#include <ESPAsyncWebServer.h>

void lobbyBegin(AsyncWebServer& server);   // регистрирует WebSocket /ws
void lobbyTick();                          // вызывать из loop() раз в пару секунд
void lobbyCleanupWs();

bool lobbyCreate(const String& tok, const String& game, const String& title, int maxP, uint16_t& id, String& err);
bool lobbyJoin(const String& tok, uint16_t id, String& err);
bool lobbyQuick(const String& tok, const String& game, uint16_t& id, String& err);
bool lobbyStart(const String& tok, String& err);
void lobbyLeave(const String& tok);

String lobbyRoomsJson(const String& game);   // [] комнат (game пусто = все)
String lobbyRoomJson(uint16_t id);           // объект комнаты или "null"
String lobbyPlayersJson();

void lobbyStats(int& players, int& rooms);
std::vector<String> lobbyPlayerLines();
std::vector<String> lobbyRoomLines();
