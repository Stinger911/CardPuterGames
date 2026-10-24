#pragma once
#include <cstdint>

// Клавиатура Cardputer ADV (контроллер TCA8418): декодирование событий и раскладка 4x14.
// Чистые функции без доступа к железу.

enum class KeyAction : uint8_t { None, Page1, Page2, Page3, Rescan };

// TCA8418 нумерует клавиши k = 10 * Tr + Tc + 1 на своей матрице 7x8; Cardputer раскладывает их в сетку 4x14:
// col = Tr * 2 + (Tc > 3), row = Tc % 4.
struct KeyEvent {
    uint8_t row = 0;
    uint8_t col = 0;
    bool pressed = false;
};

// false для 0 (FIFO пуст) и для номеров вне матрицы 7x8.
inline bool tca8418_decode(uint8_t event, KeyEvent &out) {
    uint8_t number = event & 0x7F;
    if (number == 0) return false;
    uint8_t n = number - 1;
    uint8_t tr = n / 10, tc = n % 10;
    uint8_t col = tr * 2 + (tc > 3 ? 1 : 0);
    uint8_t row = tc % 4;
    if (tc >= 8 || col >= 14) return false;
    out = {row, col, (event & 0x80) != 0};
    return true;
}

inline char key_char(uint8_t row, uint8_t col) {
    static const char kPlain[4][15] = {
        "`1234567890-=\b",
        "\tqwertyuiop[]\\",
        "\0\0asdfghjkl;'\n",
        "\0\0\0\0\0\0\0\0\0,./ ",
    };
    if (row >= 4 || col >= 14) return 0;
    return kPlain[row][col];
}

inline KeyAction key_action(char c) {
    switch (c) {
        case '1': return KeyAction::Page1;
        case '2': return KeyAction::Page2;
        case '3': return KeyAction::Page3;
        case 'r': return KeyAction::Rescan;
        default: return KeyAction::None;
    }
}
