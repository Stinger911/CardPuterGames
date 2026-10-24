# CardPutter Games — игровой сервер на M5Stack Cardputer

Cardputer поднимает Wi-Fi точку доступа, на любой телефон/ноутбук открывается captive portal
с выбором: **войти / зарегистрироваться / играть гостем**. Игры (обычные HTML/JS) раздаются по HTTP
с SD-карты, а встроенное лобби помогает игрокам найти друг друга и играть вместе (WebSocket-реле).

## Сборка и прошивка

1. Установить [PlatformIO](https://platformio.org/).
2. `pio run -t upload` (плата `m5stack-stamps3`, Cardputer подключён по USB).
3. Скопировать содержимое `sdcard_example/` в корень SD-карты (FAT32).

## Структура SD-карты

```
/config.json            необязательно: ssid, pass (>=8 симв. = WPA2), channel, max_clients, allow_guests, allow_register
/games/<id>/index.html  игра (+ любые файлы рядом: js, css, картинки, .gz подхватывается автоматически)
/games/<id>/manifest.json   {"title","description","min","max"}  (max > 1 = мультиплеер)
/www/index.html         необязательно: своя страница портала вместо встроенной
/www/assets/...         ассеты для неё, доступны по /assets/
/data/users/*.json      создаётся сервером (логины; пароли — соль + 2000 раундов SHA-256)
```

Игры доступны по `http://192.168.4.1/g/<id>/`. Каталог игр строится автоматически (кэш 10 с, клавиша `r` на устройстве — пересканировать).

## Экран / клавиши

`1` статус · `2` игроки · `3` комнаты · `r` пересканировать SD.

## Как писать мультиплеерную игру

```html
<script src="/sdk.js"></script>
<script>
  // CP.room != 0, если игра запущена из лобби (?room=ID&t=TOKEN)
  CP.on('welcome', m => { /* m.name, m.members, m.owner */ });
  CP.on('join',    m => {});            // кто-то вошёл
  CP.on('leave',   m => {});            // кто-то вышел
  CP.on('msg',     m => { /* m.from, m.d — ваши данные */ });
  CP.connect();
  CP.send({x: 10, y: 20});              // уходит всем остальным в комнате (<=1 КБ)
</script>
```

Пример — `sdcard_example/games/duel`.

## HTTP API

| Метод | Путь | Тело / ответ |
|---|---|---|
| POST | `/api/register`, `/api/login` | `{name, pass}` → `{ok, token, name}` |
| POST | `/api/guest` | → `{ok, token, name}` |
| POST | `/api/logout` | |
| GET | `/api/me`, `/api/games`, `/api/players`, `/api/rooms?game=` | |
| POST | `/api/room/create` | `{game, title, max}` |
| POST | `/api/room/join` | `{id}` |
| POST | `/api/room/quick` | `{game}` — войти в открытую комнату или создать |
| POST | `/api/room/start` | только хост, ≥2 игроков |
| POST | `/api/room/leave` | |
| GET | `/api/status` | без авторизации |
| WS | `/ws` | первым сообщением `{"t":"hello","token":"..."}` |

Токен передаётся заголовком `X-Token`, cookie `sid` или `?t=`.

## Ограничения (v0.1)

- Не проверено на железе и не компилировалось — возможны мелкие правки по API библиотек.
- Сеть открытая и трафик HTTP без шифрования — подходит для локальных игр, но не для чувствительных паролей. Используйте пароль, которым не пользуетесь где-либо ещё.
- Нет ограничения частоты попыток входа.
- До 24 одновременных сессий, 8 комнат, 8 игроков в комнате; Wi-Fi AP на ESP32 реалистично тянет 4–8 клиентов.
- Переход портал → игра держит игрока в комнате 20 с после обрыва WebSocket.
