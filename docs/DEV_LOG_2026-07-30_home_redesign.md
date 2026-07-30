# Dev Log — 2026-07-30: Home screen redesign + playback-source isolation fix

Board: Waveshare ESP32-S3-Touch-LCD-1.9 (320x170, ST7789 SPI + CST816 touch).
Branch: LVGL UI (`MiniWebRadio-Waveshare`).

This entry covers a redesign of Home's mini now-playing card and bottom nav,
and — the more important part — a root-cause fix for radio and local
playback not actually being mutually exclusive, which was corrupting what
both Now Playing pages displayed.

## 1. Home navigation changes

- Bottom-left card ("播放" → Page::NowPlaying, the old generic skeleton)
  changed to "音乐", opening `Page::LocalNowPlaying` directly.
- Tapping the mini now-playing card itself (previously inert except for its
  embedded prev/play/next buttons) now opens whichever Now Playing page
  matches what's actually playing (`onHomeNowPlayingAction`): Radio if
  `state.source == Radio`, Local if `Sd`, otherwise the old generic page.
- Bottom nav row (PLAY/RADIO/LOCAL/DECODE/SETTINGS) rebuilt as a flat
  icon+label strip instead of five bordered 56x58 cards, to free up height
  for the taller now-playing card below.

## 2. Mini now-playing card redesign

Card grew from 170x80 to 170x116 (clock panel grew to match, same height,
so the two top panels still line up). Square cover slot (reusing whichever
page last decoded real art into the shared `m_coverArtPixels`, falling back
to a generic glyph) replaced the old circular vinyl-disc-with-ring-bars
treatment. Title now scrolls (`LV_LABEL_LONG_SCROLL_CIRCULAR`) once real
content is playing.

Added a mini dot-matrix spectrum (`drawSpecDot()`, same cell-drawing
function the full-size Local/Radio spectrums use, parameterized with a
tighter column pitch / far fewer rows via new default args — full-size
call sites are unaffected).

### Two mutually exclusive sub-layouts, not one shared layout

First pass tried to fit both a "Peak/Buffer/bitrate" row (radio-only) and a
lyric line (local-only) into the same fixed y-coordinates, hiding whichever
didn't apply. This visibly fought for space — hiding a row doesn't reclaim
its vertical space for the other one, so the lyric line ended up squeezed
into a box shorter than intended. Reworked so `buildHome()` only ever
constructs *one* of the two sub-layouts, decided once at build time from
`playerService.snapshot().source`:

- **Radio**: peak-hold / buffer% / bitrate mini-metrics row (icons are
  built-in `LV_SYMBOL_*` glyphs, not custom art — this size/pitch can't
  reproduce fine line art usefully), then the spectrum, then a 9-segment VU
  ladder (mirrors Radio Now Playing's own).
- **Local/anything else**: a full-width scrolling lyric line (reusing
  `m_hasLyrics`/`playerService.currentLyricLine()`, the same synced-lyrics
  state Local Now Playing already maintains), then the spectrum, then the
  progress bar.

`refresh()` tracks which layout got built (`m_homeLayoutSource`) and calls
`show(Page::Home)` to rebuild with the other layout the moment the live
`state.source` no longer matches, so leaving radio playing and starting a
local track (or vice versa) while Home is on screen swaps layouts on its
own.

Also had to move the metrics-row/lyric-line y-position down twice during
iteration — the first placement (y=44 / y=48) still overlapped the 44px-tall
cover-art slot (y=10..54), which on a real screenshot showed the peak-dB
figure rendered directly on top of the station logo. Final positions start
at y=56/58, below the cover.

## 2a. LVGL label scroll glitch: intermittent vertical scrolling

Both the title and lyric marquee labels intermittently scrolled *vertically*
instead of (or in addition to) the intended horizontal marquee, with visible
stutters. Root-caused by reading `lv_label_refr_text()` in `lv_label.c`:

- `LV_LABEL_LONG_SCROLL_CIRCULAR` only scrolls horizontally if the measured
  text width exceeds the label's box width; if not, it falls through to a
  *second* check against box **height** and starts a vertical scroll
  animation if the text's measured height exceeds it.
- The label boxes were sized 14–16px tall. `lv_font_cjk_13`'s own
  `line_height` (baked into the font, see the generated `.c` file) is
  **17px** — taller than the box. So any time the text was short enough not
  to need horizontal scrolling, LVGL still saw `size.y > box height` and
  kicked off the unwanted vertical animation.
- Fixed by sizing both label boxes to 18px (clears the font's line height
  with 1px margin).

Separately, `refresh()` was calling `lv_label_set_text()` unconditionally
every tick regardless of whether the string actually changed.
`lv_label_set_text()` always re-measures the text and restarts the offset
animation (see `lv_label_refr_text()`), so this alone was enough to make the
marquee stutter/reset even after the height fix. Fixed by caching the last
string actually applied (`m_homeTitleLastText`/`m_homeLyricLastText`) and
only calling `set_text` on a real change.

## 3. Root-cause fix: radio/local playback weren't actually mutually exclusive

User-reported symptom: playing local music, then opening the Radio page,
showed the *local* track's title and spectrum data inside the radio page's
UI shell (and vice versa) — not because the audio was wrong, but because
both `refreshLocalNowPlaying()` and `refreshRadioNowPlaying()`
unconditionally rendered whatever the single shared `PlayerSnapshot`
currently held, regardless of which source it actually described.

Root cause: `PlayerService` only ever maintains **one** global
`PlayerSnapshot` — there's no per-source "paused in the background" state,
just one real decoder that's either playing `Sd`, `Radio`, or nothing (see
`player_service.h`'s `PlayerSource` enum). Nothing in the codebase had ever
stopped one source when the other's page was opened or a new track/station
was actually started.

Two fixes, applied at different layers on purpose:

1. **Actual playback mutual exclusion**, in `PlayerService::playRadioUrl()`
   / `playRadioStation()` / `playSdFile()` (`player_service.cpp`): each now
   calls `stop()` on itself first if the *other* source is what's currently
   active. This was **deliberately not** put in `HifiUi::show()` — an
   earlier attempt did that and was reverted after user feedback, because it
   stopped playback just from *navigating to* a page to look at it, before
   the user had actually chosen to play anything there. It belongs at the
   point playback genuinely starts, not at the point a page is opened.

2. **Page rendering isolation**, in `refreshLocalNowPlaying()` /
   `refreshRadioNowPlaying()`: each takes a copy of the incoming snapshot
   and, if `state.source` doesn't match what that page represents (`Sd` for
   local, `Radio` for radio), zeroes out `title`/`detail`/`vuLevel`/
   `spectrumBands`/position/duration (and `radioStationIndex` for the radio
   side, so the logo-reload check doesn't misfire off a stale index) before
   any of the existing rendering code runs. This is what actually makes
   "open the other page while the other source is genuinely still playing"
   show a real idle state instead of borrowed data — fix #1 alone only
   prevents the *false* case (opening a page that then starts fresh
   playback); a page can still legitimately be viewed while the *other*
   source keeps running if the user hasn't tapped play there.

## 4. Reverted: flip-clock animation for Home's clock

Attempted a simplified flip-clock effect for Home's HH/MM digits (scale-to-
zero-then-back animation via `lv_obj_set_style_transform_zoom`, paired with
a permanent seam line to fake the classic split-flap card look) plus a
paper-calendar visual treatment (cream background, red tear-strip). Caused
an immediate, reproducible crash: `Guru Meditation Error:
IntegerDivideByZero`, traced to LVGL's internal handling of a
`transform_zoom` value of exactly `0` (the animation's start/end value) —
some internal inverse-transform computation appears to divide by the zoom
factor. Fully reverted per user request (clock back to the plain dark digit
panels, no animation) rather than chasing a zoom-floor workaround this
session.

## Known limitations / follow-ups

- Home's radio-layout peak/buffer/bitrate figures share the same
  non-calibrated dB approximation as Radio Now Playing's own.
- `refreshLocalNowPlaying()`/`refreshRadioNowPlaying()`'s idle-render still
  leaves stale cover art on screen in the cross-source case (title/spectrum
  are correctly blanked, but the logo image itself isn't explicitly
  cleared) — minor, lower priority than the text/spectrum bug this fixed.
