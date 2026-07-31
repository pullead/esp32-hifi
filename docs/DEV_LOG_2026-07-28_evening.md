# Dev Log — 2026-07-28 evening → 2026-07-29

Board: Waveshare ESP32-S3-Touch-LCD-1.9 (320x170, ST7789 SPI + CST816 touch).
Branch: LVGL UI (`MiniWebRadio-Waveshare`). Continues from
`docs/DEV_LOG_2026-07-28.md` (WiFi memory fix + lyrics feature) earlier the
same day.

This entry covers: abandoning the OTA dual-partition scheme for a single
12MB app partition (and the bootloader-building saga that came with it), a
systemic font-blur fix, a handful of real bugs surfaced along the way, and a
new on-device "manually add a WiFi network" flow. Ends with one **unresolved
issue** — see the bottom.

## 1. Flash partition restructure: drop OTA, single 12MB app partition

Decision: this board never actually used network OTA (`ArduinoOTA.begin()`
had already been removed earlier), so the `ota_0`/`otadata` dual-partition
scheme was pure wasted space — and the CJK font subset (bpp=4, see §2) grew
past what the old 6MB single app slot could hold.

New layout, `boards/miniwebradio16MB_single.csv`:
```
nvs,      data, nvs,      0x9000,   16K
phy_init, data, phy,      0xF000,   4K
factory,  app,  factory,  0x10000,  12M
ffat,     data, fat,      0xC10000, 3840K
coredump, data, coredump, 0xFD0000, 192K
```
`factory` now runs from `0x10000` straight through to where `ffat` already
started (`0xC10000`), so `ffat`/`coredump` didn't need to move.
`platformio.ini`'s `[env:esp32s3_OTA]` points `board_build.partitions`
directly at this file instead of the shared `[esp32s3]` default.

**Flashing this onto a device previously running the old dual-partition
layout requires a full `esptool.py erase_flash` first** (stale
otadata/partition-table bytes aren't compatible with the new layout).

### The bootloader problem
A full chip erase wipes offset `0x0` too — previously untouched, since every
prior flash only ever wrote the app partition. This project's hybrid
`framework = arduino, espidf` PlatformIO environment turns out to **never
build a bootloader.bin at all**: confirmed empirically (`ninja -t targets
all` inside `.pio/build/esp32s3_OTA` does list a real `bootloader.bin`
target with correct dependencies, but PlatformIO's own build driver never
asks ninja to build it for this framework combination), and pure
`framework = arduino` mode uses a *precompiled* bootloader shipped in
`framework-arduinoespressif32-libs` — except only 4 variants ship
(`opi_80m`, `qio_120m`, `dio_80m`, `qio_80m`), none matching this board's
actual `dio_40m` requirement (SPI flash + PSRAM combo is only rated stable
at 40MHz, see the `[esp32s3]` section's own comment in `platformio.ini`).

Fix: built a **standalone bootloader-only PlatformIO project** in a
scratchpad dir (`framework = arduino, espidf`, matching board/flash
settings, `sdkconfig.defaults` instead of the `custom_sdkconfig` ini option
— setting `custom_sdkconfig` disables PlatformIO's automatic
`build_bootloader()` call for this exact hybrid mode, confirmed by reading
`espidf.py`'s `flag_custom_sdkonfig` gate). That produced a real,
correctly-configured `bootloader.bin` (verified via `esptool.py image-info`:
DIO, 40MHz, 16MB, valid checksum/hash) — saved permanently at
`firmware/bootloader_dio_40m_esp32s3.bin` so this never needs repeating.

Full flash sequence used: `bootloader_dio_40m_esp32s3.bin@0x0` +
`partitions.bin@0x8000` + `firmware.bin@0x10000`. Verified via serial boot
log (SD/audio/LVGL/touch/IMU all initialize, WiFi connects) and confirmed
the flash-frequency mismatch risk didn't materialize.

## 2. Font blur fix: bpp 2 → 4

Complaint: "所有界面的字体显示都发虚" (all UI text looks blurry/washed out,
like not enough ink) — systemic, not one screen. Root cause: the baked CJK
fonts (`lv_font_cjk_13.c`/`_16.c`, via `lv_font_conv`) were generated at
`--bpp 2` (4 grayscale levels of anti-aliasing) — genuinely blurry at this
resolution. Regenerated at `--bpp 4` (16 levels, crisp).

Trade-off: bpp=4 roughly doubles bitmap size for the same charset. Had to
trim `cjk_symbols.txt` first (removed the Greek/Cyrillic range and literal
pinyin tone-mark characters — unused, ~130 chars) to keep both fonts
buildable.

**Second, unrelated font bug found while doing this**: `lv_font_cjk_16.c` at
bpp=4 with the *full* shared charset (10424 chars) blew past
`lv_font_fmt_txt_glyph_dsc_t.bitmap_index`'s 20-bit field (max 1MB of
bitmap data) — indices silently wrapped, corrupting glyph lookups past that
point (`-Woverflow` caught it: "1048653 to 77"). Fix: grepped every actual
`&lv_font_cjk_16` call site in `hifi_ui.cpp`, found only 4 fixed title
strings ever use it, and built a **minimal per-size charset**
(`cjk_symbols_16.txt`, currently `台后理管置设选择网络` — 10 chars) instead
of sharing the full charset with the 13pt body font. `lv_font_cjk_13.c`
keeps the full 10424-char set (no overflow risk at that size).

**Lesson reinforced twice this session**: any time new UI text is added
that uses `lv_font_cjk_16`, its characters must be added to
`cjk_symbols_16.txt` and the font regenerated — the 13pt font's char set
does *not* cover it. Missed this once (see §6) and got tofu boxes in a new
screen's title as a direct result.

Regen command (both sizes use `lv_font_conv` 1.5.3 via the pinned Node
version):
```bash
export PATH="/Users/allen/.nvm/versions/node/v24.15.0/bin:$PATH"
TTF="/Users/allen/Documents/Esp-HiFi/DDClockPort/phase4_music_ui/tools/fonts/NotoSansSC-wght.ttf"
cd src/ui/fonts
lv_font_conv --font "$TTF" --size 16 --bpp 4 --format lvgl --lv-include lvgl.h \
  --no-compress -o lv_font_cjk_16.c --range 0x20-0x7F --symbols "$(cat cjk_symbols_16.txt)"
lv_font_conv --font "$TTF" --size 13 --bpp 4 --format lvgl --lv-include lvgl.h \
  --no-compress -o lv_font_cjk_13.c --range 0x20-0x7F --symbols "$(cat cjk_symbols.txt)"
```

## 3. `esp-dsp` API rename broke a fresh build

Unrelated to the above, surfaced only because the partition work forced the
first truly *fresh* compile of `Audio.cpp` in a long time (stale cached
`.o` had been silently reused across many previous builds). `esp-dsp` is an
unpinned managed component; whatever version the registry currently serves
renamed `dsps_biquad_sf32` → `dsps_biquad_f32` (identical signature). Fixed
with a preprocessor alias in `platformio.ini`'s common `build_flags`
(`-Ddsps_biquad_sf32=dsps_biquad_f32`) rather than patching the vendored
`ESP32-audioI2S` library, so a `lib_deps` re-fetch doesn't silently drop the
fix.

## 4. WiFi retry loop could freeze touch/spectrum for real (not just theory)

After the full erase (§1), NVS was blank, and a **pre-existing bug** in
`connectToWiFi()` bit immediately: it unconditionally overwrote WiFi slot 0
with the build-flag placeholder (`_SSID`/`_PW`, literally `"SSID"`/
`"PASSWORD"` unless `platformio_override.ini` overrides them) *every single
boot*, regardless of whether a real network was already saved there. With
zero other saved networks, `loopLvglRuntime()`'s once-a-second
`wifiMulti.run()` retry did a full blocking `WiFi.scanNetworks()` against
this un-connectable placeholder, every second, forever — long enough each
time to starve LVGL's touch polling and spectrum redraw (same loop, same
task). Symptom: touch needing 6-7 seconds to register anything, audio
unaffected (I2S/DMA doesn't need that task).

Three-part fix in `main.cpp`:
1. `connectToWiFi()` only seeds slot 0 with the placeholder if it's
   genuinely empty, not unconditionally.
2. `playerCoreWifiSavedCount()` (used by the retry guard) explicitly
   excludes the placeholder from counting as a real saved network.
3. Added exponential backoff (1s → 2s → 4s → ... capped at 16s, reset to 1s
   on success) for the retry itself, so even a **real** saved network that's
   temporarily out of range doesn't re-trigger the same freeze — this was
   originally missed and had to be added after the placeholder fix alone
   turned out insufficient (the moment a real-but-unreachable network got
   saved, the once-a-second blocking scan came right back).

## 5. Missing `esp_wifi_set_country_code` — channels 12/13 invisible

Separately, a real user WiFi network never showed up in scans despite the
phone connecting to it fine. ESP-IDF defaults to the `"01"` world-safe
regulatory domain (channels 1-11 only); routers on channel 12/13 (common
outside the US, including China) are invisible to `WiFi.scanNetworks()`
under that default. Added `esp_wifi_set_country_code("CN", true)` right
after `WiFi.mode(WIFI_STA)` in `connectToWiFi()`. (In this specific case the
network turned out to be on channel 2, so this wasn't the actual blocker
that day — but it's a real gap worth having fixed regardless, and the
network in question genuinely required rescanning to find via testing.)

## 6. `platformio_override.ini` only covered `[env:esp32s3]`, not `_OTA`

The real WiFi credentials were correctly set in the override file, but only
for the `[env:esp32s3]` section — the actual build/flash target is
`[env:esp32s3_OTA]`, which fell through to the literal placeholder every
time. Added a matching `[env:esp32s3_OTA]` block. (This file is gitignored
— contains a real password — so this fix doesn't show up in the diff.)

## 7. `setWiFiCredentials`'s "is this slot empty" check was wrong for short SSIDs

Found while debugging "add network via the phone admin page does nothing,
no error": **7 separate places** across `main.cpp` used `line.strlen() < 5`
(or `>= 5`) as a proxy for "is this NVS slot empty." Any real SSID shorter
than ~4 characters (e.g. `"s3"`, seen live in a scan) makes the *stored*
`"ssid\tpassword"` line shorter than 5 chars too, so it got treated as
empty/invalid and silently dropped — no error surfaced anywhere the user
could see (the phone page's `alert()` fires unconditionally, before the
fetch even resolves). Fixed all 7 to check `strlen() == 0` instead, which is
what "empty" actually means (`pref.putString(key, "")` is the real empty
sentinel).

## 8. New: on-device "manually add a network" flow

Restores something that existed before this WiFi UI was redesigned into
"saved list + phone-only QR admin page" (removed at the time to cut a
background scan task competing with audio/WiFi at boot). This time the scan
is strictly on-demand (only runs when the user opens this screen) and
**asynchronous** — a first attempt called `WiFi.scanNetworks()` directly
inside the LVGL button-click handler, which blocks the same task that runs
`audio.loop()`/UI ticks for several seconds and reliably froze/rebooted the
device. Fixed by moving the scan to its own one-shot FreeRTOS task
(`playerCoreWifiScanStart()`/`...InProgress()`/`...Results()` in
`main.cpp`); the UI shows "正在扫描附近WiFi..." and polls
`wifiScanInProgress()` via the existing per-tick `refreshSettingsWifi()`.

Flow: `WiFi 设置` → `手动添加` → scan-result list (SSID + RSSI, sorted
strongest-first) → tap one → full-screen (status bar hidden) password entry
with a custom on-screen keyboard → connect.

**A second real crash** here, root-caused (not guessed): LVGL's
`lv_keyboard_update_ctrl_map()` (`lv_keyboard.c`) unconditionally
`memcpy`s from whatever `ctrl_map` a custom keyboard map was given — passing
`nullptr` (assuming it meant "use defaults") is actually a straight
null-pointer read, crashing the instant the custom keyboard is first shown.
Fixed by supplying a real `lv_btnmatrix_ctrl_t[30]` array (default width 1
everywhere, backspace/enter slightly wider, space bar widest) matching the
custom map's actual button count.

Also switched to a **custom 3-row keyboard map** instead of the stock
4-row one: `lv_btnmatrix` (which `lv_keyboard` wraps) always divides its
assigned height evenly across however many rows the map declares — it does
not shrink individual rows to fit, so on this panel's ~150px keyboard
budget the stock 4th row (mostly control icons — arrows, mode toggle, OK)
came out too cramped to read even after zeroing every available
padding/font-size knob. Dropped the dedicated arrow-key row; `"1#"` (LVGL's
built-in recognized string for switching to the numeric map) does double
duty as the row-3 leading key.

## Open issue — NOT resolved, needs the next session's attention

After testing the new manual-add flow end to end (scan → pick network →
type password → tap 连接), the device was found completely hung on the
**next boot** — bootloader completes normally, but zero app-level output
ever appears (matches the pre-existing, still-unexplained
`startWifiApFallback()` hang signature documented in `main.cpp`, though that
function isn't called on this path). Reproduced twice via hardware reset.
Recovered by erasing just the NVS partition (`erase-region 0x9000 0x4000`)
and rebooting — device came back up cleanly, reconnected to its saved
network normally.

**Working theory, not confirmed**: NVS corruption from an *earlier* crash
(the null-`ctrl_map` one above) happening while a background
`wifiAddTask`/`setWiFiCredentials()` NVS write was in flight, and every
subsequent app-only reflash (never touching the NVS partition) kept
re-booting into that same corrupted state. The keyboard crash is now fixed,
so in theory this shouldn't recur — **but this was not re-verified before
the session ended** (mid-retest when the handoff to a different machine was
requested). Whoever picks this up next should:

1. Confirm current device state (was left freshly NVS-erased + reconnected,
   should be healthy).
2. Redo the exact repro: 手动添加 → pick a scan result → type a real
   password → 连接 (background task will take up to 15s trying to actually
   connect/fail before `s_wifiAddInProgress` resets — that's expected and
   shouldn't itself hang anything).
3. If it hangs again on a *fresh* NVS with no crash in between, the
   corruption theory is wrong and there's a genuine concurrent-NVS-access
   bug to find (candidate: `pref`/`Preferences` isn't documented as
   thread-safe across FreeRTOS tasks, and `setWiFiCredentials()` can now be
   entered from more than one call site).

## Files touched this session

- `boards/miniwebradio16MB_single.csv` (new)
- `firmware/bootloader_dio_40m_esp32s3.bin` (new, committed so it never
  needs rebuilding)
- `platformio.ini` — partition table pointer, `dsps_biquad_sf32` alias
- `src/lv_conf.h`, `src/ui/fonts/*` — bpp=4 regen, per-size charset split
- `src/main.cpp` — WiFi placeholder/backoff/country-code/strlen fixes,
  async scan task, `playerCoreWifiAddNetwork`/`ScanStart` bridges
- `src/ui/hifi_ui.{h,cpp}` — manual add-network screen (scan list +
  password entry + custom keyboard)
- `src/ui/player_service.{h,cpp}` — `wifiScanStart/InProgress/Results`,
  `wifiAddNetwork` bridges
