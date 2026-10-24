#pragma once
#include <cstdint>
#include <functional>

// ЖК-экран Cardputer ADV 240x135. Кадр рисуется полосами (лимит DMA 32 КБ), поэтому весь интерфейс
// отрисовывается заново для каждой полосы, а примитивы обрезаются по ней.
namespace gfx {
constexpr int W = 240, H = 135;
// RGB565 «как на панели»
constexpr uint16_t Black = 0x0000, White = 0xFFFF, Green = 0x07E0, Red = 0xF800, Cyan = 0x07FF, Grey = 0x8410;

void rect(int x, int y, int w, int h, uint16_t c);
void text(const char* s, int x, int y, uint16_t c, int scale = 1);   // шрифт только заглавный; y — верх строки
}  // namespace gfx

bool display_init();
void display_frame(const std::function<void()>& draw);
