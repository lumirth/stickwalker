#pragma once
#include "infrared/diode.h"
#include <stdbool.h>
#include <stdint.h>

enum { SCREEN_W = 240, SCREEN_H = 135, PALETTES = 6 };
extern char palette_names[PALETTES][16];

void display_palette_table(uint16_t table[PALETTES][4], bool write);
void display_style(unsigned palette, bool reverse, unsigned brightness);
void display_fill(unsigned shade);
void display_rect(int x, int y, int w, int h, unsigned shade);
void display_text(int x, int y, const char *text, unsigned shade);
void display_game(const uint8_t *pixels);
void display_power(bool on);
uint64_t display_tick(void);

void display_diode(DiodeIcon icon);
