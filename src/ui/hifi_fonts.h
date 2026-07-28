#pragma once

#include "lvgl.h"

// Subsetted Noto Sans SC fonts (GB2312 common set + ASCII), generated with
// lv_font_conv into src/ui/fonts/. 2bpp, uncompressed (faster render).
//   lv_font_cjk_16 — titles / headers
//   lv_font_cjk_13 — body, labels, list rows, settings
// Rare glyphs outside GB2312 fall back to .notdef; upgrade to an on-SD font
// later if arbitrary track titles need full coverage. See docs/UI_DESIGN_SPEC.md.
LV_FONT_DECLARE(lv_font_cjk_16);
LV_FONT_DECLARE(lv_font_cjk_13);
