#pragma once

#include "lvgl.h"

// Subsetted Noto Sans SC fonts, generated with lv_font_conv into src/ui/fonts/.
// Both current fonts are 4bpp and uncompressed for sharper edges on the
// 320x170 ST7789 panel. lv_font_cjk_13 carries the broad dynamic charset;
// lv_font_cjk_16 is intentionally limited to fixed UI title glyphs to avoid
// LVGL 8 font bitmap-index overflow.
LV_FONT_DECLARE(lv_font_cjk_16);
LV_FONT_DECLARE(lv_font_cjk_13);

// Semantic aliases for new UI work. Existing screens still contain many direct
// font references; keep these names as the migration target for the global
// readability pass after the on-device font preview is reviewed.
#define HIFI_FONT_TITLE (&lv_font_cjk_16)
#define HIFI_FONT_BODY (&lv_font_cjk_13)
#define HIFI_FONT_CAPTION (&lv_font_cjk_13)
#define HIFI_FONT_DYNAMIC_TEXT (&lv_font_cjk_13)
#define HIFI_FONT_STATUS (&lv_font_montserrat_10)
#define HIFI_FONT_ICON (&lv_font_montserrat_16)
