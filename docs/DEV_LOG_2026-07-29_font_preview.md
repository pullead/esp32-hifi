# DEV LOG 2026-07-29: Font preview handoff

## Branch

`codex/font-preview-ab`

Base branch before this work: `lvgl-ui` at `4b4bd9c chore: document portable build workflow`.

## Goal

The current LVGL UI is readable but still looks soft/fuzzy on the physical
Waveshare ESP32-S3-Touch-LCD-1.9 panel. The requested next step was not a blind
global font replacement. Instead, we started a controlled A/B preview so the
actual board can show whether the blur is caused mainly by font bitmap quality,
low contrast colors, faux-bold rendering, or LVGL's built-in scrolling label
animation.

## Changes Made

### 1. Added on-device font preview page

Files:

- `src/ui/hifi_ui.h`
- `src/ui/hifi_ui.cpp`

Added a new page enum: `Page::FontPreview`.

Settings now has a second card labeled `字体`; tapping it opens a font clarity
preview page. The preview compares:

- `CJK13`: current normal `lv_font_cjk_13`
- `CJK13+B`: same font drawn twice with 1 px horizontal offset
- `DIM`: low-contrast `kInkDim`
- `PURPLE`: lyric-like `kAccentBright`
- `SCROLL`: LVGL `LV_LABEL_LONG_SCROLL_CIRCULAR`
- `CJK16`: current fixed-glyph title font

This is intended for real-panel visual judgement before touching all screens.

### 2. Added semantic font aliases

File:

- `src/ui/hifi_fonts.h`

The file now documents the real current state:

- fonts are 4bpp, not 2bpp
- `lv_font_cjk_13` is the broad dynamic charset
- `lv_font_cjk_16` is intentionally small/fixed to avoid LVGL 8 bitmap-index
  overflow

Added migration-target aliases:

- `HIFI_FONT_TITLE`
- `HIFI_FONT_BODY`
- `HIFI_FONT_CAPTION`
- `HIFI_FONT_DYNAMIC_TEXT`
- `HIFI_FONT_STATUS`
- `HIFI_FONT_ICON`

Existing screens still use many direct font references. The aliases are for the
next global readability pass after the preview is reviewed.

### 3. Added portable font generation scripts

Files:

- `scripts/gen_fonts_windows.ps1`
- `scripts/gen_fonts_mac.sh`

Both scripts regenerate the existing font pair:

- `lv_font_cjk_13.c` from `cjk_symbols.txt`
- `lv_font_cjk_16.c` from `cjk_symbols_16.txt`

Both default to 4bpp and avoid regenerating the 16 px font with the full dynamic
charset.

Windows script default font search:

1. `tools\fonts\NotoSansSC-VF.ttf`
2. `C:\Windows\Fonts\NotoSansSC-VF.ttf`

macOS script expects a font path argument or `tools/fonts/NotoSansSC-VF.ttf`.

### 4. Updated font README

File:

- `src/ui/fonts/README.md`

The README now describes the current 4bpp setup, the on-device preview page,
Windows/macOS regeneration commands, and the known LVGL 8 16 px font-size
constraint.

## Verification

Completed:

- `hifi_ui.cpp` was compiled by PlatformIO during the attempted build; the
  generated object file timestamp is newer than the source timestamp.
- `git diff --check` passed.
- `scripts/gen_fonts_windows.ps1` was executed far enough to fail at the
  expected `lv_font_conv not found` dependency check, confirming PowerShell
  parsing is valid.
- No PlatformIO/gcc build process is intentionally left running after the pause.

Not completed:

- Full firmware link/build did not complete in this Codex sandbox session.

Reason:

- A sandboxed PlatformIO run failed with:
  `PermissionError: [Errno 13] Permission denied: 'C:\\Users\\tei_s\\.platformio\\platforms.lock'`
- An earlier non-escalated attempt also ran for a long time and appeared to stall
  around unrelated `DLNAClient.cpp` compilation. It never reached `firmware.elf`.

## Handoff Steps For Home Machine

1. Fetch branch:

   ```bash
   git fetch origin codex/font-preview-ab
   git switch codex/font-preview-ab
   ```

2. Build normally on the home development environment:

   ```bash
   pio run -e esp32s3_OTA -j 1
   ```

3. Flash only after a successful build.

4. On the device, open:

   `设置 -> 字体`

5. Compare the preview rows on the real panel:

   - If `CJK13+B` looks clearly sharper, the next pass should replace faux-bold
     duplicated labels with a real Medium/Bold generated CJK font.
   - If `DIM` is the worst row, raise secondary text contrast globally.
   - If `SCROLL` looks significantly worse than static rows, replace local
     Now Playing's `LV_LABEL_LONG_SCROLL_CIRCULAR` title with an integer-pixel
     manual marquee.
   - If `CJK16` is noticeably clearer but too charset-limited, investigate a
     14/15 px Medium dynamic font first before trying a full 16 px dynamic font.

## Recommended Next Development Step

Do not globally replace fonts yet. First build and flash this branch, inspect
the preview page on the physical Waveshare 1.9 inch display, then choose one of
these targeted fixes:

1. Generate 14/15 px Medium CJK dynamic font.
2. Raise `kInkDim` / `kInkFaint` contrast for readable text.
3. Replace LVGL circular scrolling labels with integer-pixel marquee for local
   music title scrolling.
4. Migrate direct font references in `hifi_ui.cpp` to the semantic aliases in
   `hifi_fonts.h`.
