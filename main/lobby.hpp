#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "esp_http_server.h"

void lobbyBegin(httpd_handle_t server);    // регистрирует WebSocket /ws
void lobbyTick();                          // вызывать из главного цикла раз в пару секунд
void lobbyOnSocketClosed(int fd);          // из close_fn http-сервера

bool lobbyCreate(const std::string& tok, const std::string& game, const std::string& title, int maxP, uint16_t& id, std::string& err);
bool lobbyJoin(const std::string& tok, uint16_t id, std::string& err);
bool lobbyQuick(const std::string& tok, const std::string& game, uint16_t& id, std::string& err);
bool lobbyStart(const std::string& tok, std::string& err);
void lobbyLeave(const std::string& tok);

std::string lobbyRoomsJson(const std::string& game);   // [] комнат (game пусто = все)
std::string lobbyRoomJson(uint16_t id);                // объект комнаты или "null"
std::string lobbyPlayersJson();

void lobbyStats(int& players, int& rooms);
std::vector<std::string> lobbyPlayerLines();
std::vector<std::string> lobbyRoomLines();
