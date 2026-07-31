#pragma once
#include <stdint.h>

// Raw RGB565 pixel data (LV_COLOR_16_SWAP=0), generated from an HTML/CSS
// mockup rendered with headless Chrome -- see docs/cassette-design/ for the
// source files and regeneration steps. Built into an lv_img_dsc_t at
// runtime in hifi_ui.cpp (same pattern as the decoded cover art), not as a
// static const struct literal, to avoid relying on designated-initializer
// support for lv_img_dsc_t's nested bitfield header.
extern const uint16_t cassette_body_w;
extern const uint16_t cassette_body_h;
extern const uint8_t cassette_body_map[];

extern const uint16_t cassette_reel_w;
extern const uint16_t cassette_reel_h;
extern const uint8_t cassette_reel_map[];
