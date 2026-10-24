#include "lcd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "font5x7.hpp"
#include "m5stack-cardputer.hpp"

// Особенности платы (см. m5sPlaud/firmware/README.md):
//  - подсветка (GPIO38) только прямым GPIO, LEDC из espp на ADV не светит;
//  - один DMA-перенос <= 32 КБ, кадр уходит полосами по 27 строк;
//  - драйвер переставляет байты в буфере, поэтому буферы хранят «родной» RGB565 и чередуются;
//  - панель стартует с инвертированными цветами, put() инвертирует пиксели.
namespace {
const char* TAG = "display";
constexpr size_t kStripRows = 27;
constexpr size_t kStripBytes = gfx::W * kStripRows * 2;
static_assert(gfx::H % kStripRows == 0, "strips must tile the frame");
static_assert(kStripBytes <= 32768, "strip exceeds the SPI DMA limit");

alignas(16) std::array<uint8_t, kStripBytes> g_storage[2];
uint8_t* g_frame = g_storage[0].data();
unsigned g_count = 0;
int g_strip_y = 0;

void put(int x, int y, uint16_t c) {
  y -= g_strip_y;
  if (x < 0 || y < 0 || x >= gfx::W || y >= static_cast<int>(kStripRows)) return;
  size_t i = (static_cast<size_t>(y) * gfx::W + static_cast<size_t>(x)) * 2;
  c = ~c;
  g_frame[i] = c & 0xFF;
  g_frame[i + 1] = c >> 8;
}

const Glyph* find_glyph(char ch) {
  ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  for (const auto& g : kGlyphs)
    if (g.ch == ch) return &g;
  return nullptr;
}
}  // namespace

namespace gfx {
void rect(int x, int y, int w, int h, uint16_t c) {
  int j0 = std::max(0, g_strip_y - y), j1 = std::min(h, g_strip_y + static_cast<int>(kStripRows) - y);
  for (int j = j0; j < j1; ++j)
    for (int i = 0; i < w; ++i) put(x + i, y + j, c);
}

void text(const char* s, int x, int y, uint16_t c, int scale) {
  const int gap = std::max(1, scale / 2), adv = 5 * scale + gap;
  for (int k = 0; s[k]; ++k, x += adv) {
    const Glyph* g = find_glyph(s[k]);
    if (!g) continue;   // пробел и неизвестные символы (в т.ч. кириллица)
    for (int r = 0; r < 7; ++r)
      for (int col = 0; col < 5; ++col)
        if (g->rows[r] & (1 << (4 - col))) rect(x + col * scale, y + r * scale, scale, scale, c);
  }
}
}  // namespace gfx

bool display_init() {
  auto& cp = espp::M5StackCardputer::get();
  if (!cp.initialize_lcd()) {
    ESP_LOGE(TAG, "LCD init failed");
    return false;
  }
  gpio_reset_pin(GPIO_NUM_38);
  gpio_set_direction(GPIO_NUM_38, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_38, 1);
  // Синхронная очистка сначала: она сбрасывает транзакцию, оставшуюся после инициализации панели.
  cp.display_driver()->clear(0, 0, gfx::W, gfx::H, 0xFFFF);
  return true;
}

void display_frame(const std::function<void()>& draw) {
  auto& cp = espp::M5StackCardputer::get();
  for (size_t y = 0; y < gfx::H; y += kStripRows) {
    g_strip_y = static_cast<int>(y);
    g_frame = g_storage[g_count++ & 1].data();
    std::memset(g_frame, 0xFF, kStripBytes);   // инвертированный чёрный
    draw();
    cp.write_lcd_frame(0, y, gfx::W, kStripRows, g_frame);
  }
}
