# CJK fonts for the LVGL UI

The firmware currently ships two generated LVGL 8 fonts:

- `lv_font_cjk_13.c`: broad dynamic CJK/ASCII/fullwidth subset for labels,
  lists, station names, track titles, lyrics, and WiFi SSIDs.
- `lv_font_cjk_16.c`: small fixed-glyph subset for stable UI titles. Do not
  regenerate this with the full `cjk_symbols.txt` set unless
  `LV_FONT_FMT_TXT_LARGE` is intentionally enabled and tested.

Both fonts are generated as **4bpp, uncompressed** bitmaps. This costs more
flash than 2bpp, but gives sharper antialiasing on the 320x170 ST7789 panel.

## On-device preview

The Settings screen contains a `字体` card that opens a font clarity preview.
Use it before changing the global font mapping. The preview compares:

- normal `lv_font_cjk_13`
- faux-bold `lv_font_cjk_13` drawn twice with a 1 px offset
- low-contrast text
- purple lyric text
- LVGL circular scrolling title text
- fixed-glyph `lv_font_cjk_16`

The goal is to identify whether the blur is mainly from the font bitmap,
contrast, or scrolling behavior before replacing fonts globally.

## Regenerate on Windows

Install `lv_font_conv` once:

```powershell
npm install -g lv_font_conv@1.5.3
```

Then run from the project root:

```powershell
.\scripts\gen_fonts_windows.ps1
```

By default the script uses `tools\fonts\NotoSansSC-VF.ttf` when present, then
falls back to `C:\Windows\Fonts\NotoSansSC-VF.ttf`. To test a heavier font:

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
./scripts/gen_fonts_mac.sh ./tools/fonts/NotoSansSC-VF.ttf
```

`LV_FONT_CONV` and `BPP` can be overridden:

```bash
LV_FONT_CONV=/path/to/lv_font_conv BPP=4 ./scripts/gen_fonts_mac.sh ./tools/fonts/NotoSansSC-VF.ttf
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
