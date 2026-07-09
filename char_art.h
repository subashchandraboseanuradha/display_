// Mascot walk-cycle frames (4), magenta-keyed transparency.
// Pixel data is PRE-BYTE-SWAPPED RGB565 (project pushImage convention).
#pragma once
#include <stdint.h>

#define CHAR_ART_W 64
#define CHAR_ART_H 100
// Magenta 0xF81F, stored byte-swapped like the pixel data
#define CHAR_KEY_SW 0x1FF8

extern const uint16_t char_walk_0[6400];
extern const uint16_t char_walk_1[6400];
extern const uint16_t char_walk_2[6400];
extern const uint16_t char_walk_3[6400];
