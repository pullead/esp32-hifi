# CJK fonts for the LVGL UI

The firmware currently ships two generated LVGL 8 fonts:

- `lv_font_cjk_13.c`: broad dynamic CJK/ASCII/fullwidth subset for labels,
  lists, station names, track titles, lyrics, and WiFi SSIDs.
- `lv_font_cjk_16.c`: small fixed-glyph subset for stable UI titles. Do not
  regenerate this with the full `cjk_symbols.txt` set unless
  `LV_FONT_FMT_TXT_LARGE` is intentionally enabled and tested.

Both fonts are generated as **4bpp, uncompressed** bitmaps. This costs more
flash than 2bpp, but gives sharper antialiasing on the 320x170 ST7789 panel.

Both are baked from `tools/fonts/NotoSansSC-Medium500.ttf` -- a static
**wght=500 (Medium)** instance, not the raw variable `NotoSansSC-*.ttf`
family font. This matters: `lv_font_conv` given a variable font uses
whatever its *default* named instance is when no weight is pinned, and
this family's default instance is literally named "Thin" (wght=100).
2026-07-29's systemic "whole UI looks washed out" complaint turned out to
be exactly this -- every font baked straight from the variable font all
session had silently been Thin, confirmed via an on-device A/B (see
`docs/DEV_LOG_2026-07-29_font_preview.md`) against a real Medium instance
generated with `fontTools.varLib.instancer`. If you ever need to
re-instance a different weight from the original variable font:

```python
from fontTools.varLib import instancer
from fontTools.ttLib import TTFont
f = TTFont("NotoSansSC-wght.ttf")
instancer.instantiateVariableFont(f, {"wght": 500}).save("NotoSansSC-Medium500.ttf")
```

## On-device preview

The Settings screen contains a `字体` card that opens a font clarity preview.
Use it before changing the global font mapping again. The preview compares:

- normal `lv_font_cjk_13` (now Medium weight)
- low-contrast text
- purple lyric text
- LVGL circular scrolling title text
- fixed-glyph `lv_font_cjk_16`

The goal is to identify whether any remaining blur is from contrast or
scrolling behavior before changing fonts/colors globally again.

## Regenerate on Windows

Install `lv_font_conv` once:

```powershell
npm install -g lv_font_conv@1.5.3
```

Then run from the project root:

```powershell
.\scripts\gen_fonts_windows.ps1
```

By default the script uses `tools\fonts\NotoSansSC-Medium500.ttf` when present, then
falls back to `C:\Windows\Fonts\NotoSansSC-Medium500.ttf`. To test a heavier font:

```powershell
.\scripts\gen_fonts_windows.ps1 -FontPath C:\Windows\Fonts\msyhbd.ttc
```

## Regenerate on macOS

Install `lv_font_conv` once:

```bash
npm install -g lv_font_conv@1.5.3
```

Then run from the project root:

```bash
./scripts/gen_fonts_mac.sh ./tools/fonts/NotoSansSC-Medium500.ttf
```

`LV_FONT_CONV` and `BPP` can be overridden:

```bash
LV_FONT_CONV=/path/to/lv_font_conv BPP=4 ./scripts/gen_fonts_mac.sh ./tools/fonts/NotoSansSC-Medium500.ttf
```

## Known constraints

- The 16 px full dynamic CJK font overflowed LVGL 8's default font bitmap
  index field in earlier testing. Keep 16 px as a fixed UI-title subset until
  a separate `LV_FONT_FMT_TXT_LARGE` experiment is built and flash impact is
  measured.
- Font source files are not committed here yet. If a font is vendored under
  `tools/fonts/`, verify its license before committing it.
- The first build after changing generated font C files can be slow because
  `lv_font_cjk_13.c` is multi-megabyte source.
