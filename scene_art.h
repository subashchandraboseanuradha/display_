// Parallax screensaver scenery: warm pixel-art sky/hills + ground strip.
// Both tile horizontally (edges crossfaded). PRE-BYTE-SWAPPED RGB565.
#pragma once
#include <stdint.h>

#define SCENE_BG_W 480
#define SCENE_BG_H 240
#define SCENE_GND_W 480
#define SCENE_GND_H 52

extern const uint16_t scene_bg[480*240];
extern const uint16_t scene_ground[480*52];
