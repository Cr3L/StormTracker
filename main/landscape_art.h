#pragma once

#include <stdint.h>

#include "landscape.h"

#define LANDSCAPE_ART_SIZE 120

void landscape_art_render(uint16_t *pixels, const landscape_scene_t *scene,
                          uint32_t elapsed_ms, int wind_mph,
                          int daylight_progress);
