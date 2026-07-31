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

## Recommended Next Development Step (as of the Codex handoff)

Do not globally replace fonts yet. First build and flash this branch, inspect
the preview page on the physical Waveshare 1.9 inch display, then choose one of
these targeted fixes:

1. Generate 14/15 px Medium CJK dynamic font.
2. Raise `kInkDim` / `kInkFaint` contrast for readable text.
3. Replace LVGL circular scrolling labels with integer-pixel marquee for local
   music title scrolling.
4. Migrate direct font references in `hifi_ui.cpp` to the semantic aliases in
   `hifi_fonts.h`.

---

## RESOLVED on the home machine, same day

Merged this branch's 6 commits onto the continuing `lvgl-ui` line (cherry-pick,
one trivial `.gitignore` conflict), vendored `Arduino_GFX` into `lib/` to match
the portability fix, and did a full clean build -- **SUCCESS, zero errors**,
first time this code has actually compiled anywhere. Flashed and verified:

- The WiFi mutex hardening (`s_prefMutex`/`s_wifiOpMutex`) fixed the exact
  unresolved boot-hang issue from `docs/DEV_LOG_2026-07-28_evening.md` --
  user confirmed the manual-add-network flow no longer hangs the device.
- Then used the font preview page for its actual intended purpose: real
  root cause found. `NotoSansSC-wght.ttf`'s **unpinned/default variable-font
  instance is literally named "Thin" (wght=100)** -- confirmed via `fontTools`
  (`TTFont(...).name` table, nameID 17 = "Thin"). Every font baked from this
  file all session (both `lv_font_cjk_13` and `_16`, at any bpp) had silently
  been Thin the entire time. This is very plausibly the real cause of the
  original, very first "whole UI looks washed out like not enough ink"
  complaint from earlier in this project, not an anti-aliasing/bpp issue as
  assumed back then.

  Verification: instantiated a real static wght=500 (Medium) TTF with
  `fontTools.varLib.instancer`, baked a throwaway preview-only font from it,
  added a `MEDIUM` row to the on-device preview page next to plain `CJK13`.
  On the real panel, `MEDIUM` was unambiguously sharper than `CJK13` and
  slightly cleaner than the `CJK13+B` faux-bold hack (no double-draw
  ghosting on stroke edges). User confirmed the same conclusion independently
  from the photo.

  Applied: regenerated both `lv_font_cjk_13.c` (full `cjk_symbols.txt`
  charset) and `lv_font_cjk_16.c` (`cjk_symbols_16.txt`) from the Medium
  instance, at 4bpp, same as before -- no `bitmap_index` overflow. The Medium
  static TTF itself is committed at `tools/fonts/NotoSansSC-Medium500.ttf`
  (10.6MB) so this is reproducible on either machine without needing
  `fontTools`/`instancer` again; `gen_fonts_mac.sh`/`gen_fonts_windows.ps1`
  and `src/ui/fonts/README.md` were updated to point at it by default instead
  of the raw variable font, so this exact mistake (baking straight from an
  unpinned variable font) can't quietly recur.

  Also bumped `kInkDim` (0x9AA0B4 → 0xB0B6C8) since it was independently
  confirmed as the lowest-contrast row in both A/B rounds.

  Removed the now-redundant `CJK13+B`/`MEDIUM` comparison rows from the
  preview page (the base font itself is Medium now, they'd render
  identically to `CJK13`); kept `DIM`/`PURPLE`/`SCROLL`/`CJK16` since those
  questions are still open for a future pass.

User confirmed on real hardware after flashing: "好多了，清晰多了" (much
better, much clearer) across lyrics/menus/local-music-list screens.

**Still open / not addressed this round**: `SCROLL` row (LVGL circular
scrolling label quality while in motion) and whether `HIFI_FONT_*` semantic
aliases in `hifi_fonts.h` should replace the remaining direct
`&lv_font_cjk_13`/`&lv_font_cjk_16` references scattered through
`hifi_ui.cpp` -- neither was revisited this round since the weight fix alone
resolved the reported complaint.
