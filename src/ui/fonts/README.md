# CJK fonts (LVGL 8, subsetted)

`lv_font_cjk_16.c` / `lv_font_cjk_13.c` are Noto Sans SC subsetted to the
**GB2312 common set (~6763 hanzi) + ASCII + fullwidth forms** (7444 glyphs),
2bpp, uncompressed. Declared in `../hifi_fonts.h`. Compiled size ≈ 380 KB
total (both sizes). Covers ~99.7% of modern Chinese; rare glyphs fall back
to `.notdef`.

## Regenerate

```bash
npm install -g lv_font_conv           # v1.5.3 used
# GB2312 charset -> cjk_symbols.txt (see the python snippet in git history / DEV notes)
TTF=../../../DDClockPort/phase4_music_ui/tools/fonts/NotoSansSC-wght.ttf
for SZ in 16 13; do
  lv_font_conv --font "$TTF" --size $SZ --bpp 2 --format lvgl \
    --lv-include lvgl.h --no-compress -o lv_font_cjk_$SZ.c \
    --range 0x20-0x7F --symbols "$(cat cjk_symbols.txt)"
done
```

Note: these are multi-MB C arrays, so the **first** build after adding/changing
them is slow (~9 min). Subsequent builds reuse the cached `.o` and are fast.

Upgrade path for full coverage: load a full CJK font from SD via FreeType /
`lv_font_load` instead of subsetting. See `docs/UI_DESIGN_SPEC.md`.
