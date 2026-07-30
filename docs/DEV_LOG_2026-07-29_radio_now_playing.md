# Dev Log — 2026-07-29: Radio Now Playing feature

Board: Waveshare ESP32-S3-Touch-LCD-1.9 (320x170, ST7789 SPI + CST816 touch).
Branch: LVGL UI (`MiniWebRadio-Waveshare`).

This entry covers building a dedicated Radio Now Playing screen (previously
radio shared the generic `buildMediaPage()` skeleton), the station-logo
download/cache pipeline that feeds it, a cassette alternate view, real L/R
peak metering, and a first batch of curated radio stations with real logos.

## 1. Radio Now Playing screen

New `HifiUi::buildRadioNowPlaying()` / `refreshRadioNowPlaying()`, mirroring
`buildLocalNowPlaying()`'s flat-card layout but swapping out everything that
doesn't make sense for a live stream:

- Square station-logo slot instead of local's circular vinyl disc (real art
  reuses `m_coverArtPixels`/`m_coverArtDsc`, the same buffer local cover art
  already uses — radio and local are never on screen together).
- 9-segment VFD-style level meter (green→yellow→red) instead of elapsed/
  total time, driven by `state.vuLevel`.
- Buffer-fill percentage redrawn as a thin 2-row green dot-matrix strip
  instead of a plain `lv_bar` (radio can't seek, so no draggable slider).
- Dot-matrix spectrum unchanged from local's (same `drawSpecDot()` cells).
- Control bar: bottom-left slot repurposed from local's play-mode cycle
  button to an inert "EQ" placeholder (no audio EQ wired up yet); the slot
  left of the cassette toggle repurposed from local's playlist button to
  "已保存的网络电台" (saved stations list).
- Title line scrolls the station name; the line below it scrolls the live
  ICY StreamTitle (`state.detail` — `ESP32-audioI2S` already parses this,
  no new plumbing needed).

### Bug: stack overflow on first tap into a station with no cached logo

`loadRadioIcon()` called `clearRadioIcon()` (which zeroes
`m_currentRadioIconIndex`) and then returned early on decode failure
*without* setting `m_currentRadioIconIndex` back to the station index.
`refreshRadioNowPlaying()`'s "did the station change" check
(`state.radioStationIndex != m_currentRadioIconIndex`) then never resolved,
so every refresh tick called `show(Page::Radio)` again, which rebuilt the
page and re-entered the same broken path — unbounded recursion overflowing
`loopTask`'s stack within about a second. Confirmed via serial capture
(`***ERROR*** A stack overflow in task loopTask has been detected`). Fixed
by setting `m_currentRadioIconIndex = stationIndex` unconditionally at the
top of `loadRadioIcon()`, success or not.

### Bug: dark "picture frame" border around small logos

The cover-art wrap objects (`m_coverArtWrap` in Radio Now Playing, and the
28x28 row icons in the saved-stations list) had an opaque `kPanelDeep`
background. Downloaded logos usually decode much smaller than their 72x72 /
28x28 slot (JPEG downscaling via `scaleFactor=8`), so the opaque background
showed through as a dark border around a small centered image. Fixed by
making the wraps transparent and using `lv_img_set_zoom()` to fill the slot
(longer side scaled to the slot size, `clip_corner` crops the overflow),
which also keeps the image visually aligned with the VFD ladder below it.

## 2. Station-logo download/cache pipeline

New in `main.cpp`: `radioIconSyncStart()` walks every saved station missing
a cached `/logo/radio_<index>.jpg`, one at a time on its own FreeRTOS task
(never synchronously from an LVGL click handler — a direct
`WiFi.scanNetworks()` call in a button handler was already proven to freeze
the device earlier in the project). Triggered on-demand when the saved-
stations list is opened, not at boot.

Two-level fallback when Radio-Browser's own `favicon` field is missing or
won't decode (radio-browser's favicons are frequently PNG/ICO, which the
JPEG-only `decodeJpgFromMemory()` can't handle):

1. **Retry once** on the real favicon URL (covers transient TLS/network
   hiccups — confirmed via `[ICON]` debug logging that two stations which
   failed once succeeded on a second attempt with no code change needed).
2. **Themed stock photo** via `loremflickr.com/300/300/<keyword>`, always a
   real JPEG. Keyword picked from Radio-Browser's own `tags` field first,
   falling back to the first ASCII word in the station name, falling back
   to `"radio"`.
3. If both fail: **offline gradient monogram tile**, drawn locally with no
   network — first UTF-8 character of the station name on a color/gradient
   picked deterministically from a hash of the full name (`stationMonogram()`
   in `hifi_ui.cpp`), so the same station always gets the same tile. Radio
   Now Playing's version wraps the full name across a few lines; the list
   row's smaller tile keeps just the single leading character.

Two real bugs found via the `[ICON]` logging added for this and fixed:

- `httpDownloadToFile()` had no `setFollowRedirects()` call —
  `loremflickr.com` serves the actual image via a 301/302, and
  `HTTPClient` defaults to `HTTPC_DISABLE_FOLLOW_REDIRECTS`, so every theme-
  photo fetch was silently failing with what looked like a network error.
  Fixed with `HTTPC_FORCE_FOLLOW_REDIRECTS`.
- `pickThemeKeyword()` could return a multi-word tag (e.g. `"classic jazz"`)
  with an un-escaped space, producing an invalid URL path and a 400 from
  loremflickr. Fixed to only ever take the first single word of whichever
  source (tag or name) it falls back to.

Also fixed: the saved-stations list's "is this row the currently playing
station" check was hardcoded `i == 1` (station 1 always showed as active);
now compares `state.radioStationIndex` from a real snapshot.

## 3. Cassette alternate view for radio

`buildCassetteVisual()` (previously local-only, full-screen skeuomorphic
tape art with real bitmap reels, edge-swipe-only exit) turned out to be
already source-agnostic — it only ever reads `state.title`/`state.detail`
via the shared `m_title`/`m_detail` widgets. Reused as-is for radio: new
`m_radioCassetteView` flag, `onRadioViewToggleAction`, and the bottom-right
control-bar slot (previously an inert Settings shortcut) now toggles it.
`refreshRadioNowPlaying()` gained the same needle-deflection/reel-spin
driving logic `refreshLocalNowPlaying()` already had, copied verbatim since
both read the same `state.spectrumBands`.

## 4. Real L/R peak-hold meters

`Audio::getVUlevel()` already returns both channels packed into one
`uint16_t` (low byte left, high byte right) — the project had been storing
this straight into a `uint8_t vuLevel` field, silently truncating to just
the left channel and discarding the right one entirely. Split explicitly:
`PlayerSnapshot::vuRight` added, `main.cpp`'s snapshot read now unpacks
both bytes. Radio Now Playing's peak-hold row (below the VFD ladder, above
the buffer bar) shows both channels (`L -6.2  R -3.1` style), each with
independent hold/decay state. The dB figure is `20*log10(raw/255)`, an
approximation for display purposes — `getVUlevel()` has no real dBFS
calibration.

## 5. First batch of curated stations + real logos

Radio-Browser's own database is sparse/unreliable for Chinese domestic
stations (mostly HLS-only or dead links). Settled on the 蜻蜓FM (qtfm.cn)
CDN for stable direct MP3 streams the on-device decoder can actually play
(no HLS support). Added, with logos sourced from Wikipedia infobox images /
official CDNs (not Radio-Browser, which mostly returns unrelated theme
photos or nothing for these): 中国之声, 经济之声, 音乐之声, 经典音乐广播,
大连都市之声, 内蒙古音乐之声, 广东音乐之声, 动感101, Love Radio 103.7,
经典947. All logos re-encoded to baseline (not progressive) JPEG — the
on-device `tjpgd`-based decoder doesn't handle progressive JPEG — and
resized to a consistent ~320px long edge before uploading via the device's
existing FTP server.

## Known limitations / follow-ups

- Peak dB readout is an approximation, not a calibrated measurement.
- The theme-photo fallback depends on a third-party public service
  (loremflickr.com) with no SLA; offline monogram tile is the real safety
  net if it's ever unavailable.
- EQ control-bar slot is still a visual placeholder — no audio EQ wired up.
