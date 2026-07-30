# 2026-07-30 Audio Settings / EQ Handoff

## Current Board State

- The physical ESP32-S3 board has been restored to the previously backed up full-flash image.
- Restore command wrote the full 16MB backup image from address `0x0`.
- Backup image:
  - `backups/board_flash_20260730_141506/esp32s3_full_flash_16mb.bin`
  - Size: `16777216` bytes
- esptool result:
  - Chip: ESP32-S3
  - Port: `COM5`
  - Flash write completed
  - Hash verified
  - Hard reset completed

The board is therefore no longer running today's unfinished audio settings UI experiment.

## Git / Source State Before Handoff

- Branch: `feature/company-eq`
- Base commit before today's WIP: `c7d592c`
- Working tree currently has uncommitted WIP changes in:
  - `src/ui/hifi_ui.cpp`
  - `src/ui/hifi_ui.h`

No final commit was made for this WIP because the user paused the work and requested the board be restored first.

## Why This WIP Exists

The first EQ implementation was rejected because it did not follow the planned audio settings information architecture closely enough. Main feedback:

- Audio settings must not be a single standalone EQ page.
- The settings hierarchy should be:
  - Settings
  - Audio
  - Decode / Output
  - EQ / Effects
  - DAC / Amp
- Audio settings screens should not use the normal global status bar.
- The right-side selected preset label was redundant and should be removed.
- Slider controls were visually too large and too thick.
- Decode/output and effects pages from the design plan need separate screens.
- Keep the background dark/black.

## WIP UI Changes Made Today

The current uncommitted WIP expands `HifiUi::Page` from a single `AudioEq` page into a real audio settings page tree:

- `AudioHome`
- `AudioDecode`
- `AudioOutputDetails`
- `AudioOutputPolicy`
- `AudioEq`
- `AudioEqBand`
- `AudioEffects`
- `AudioDac`

New UI builders were added:

- `buildAudioTopBar()`
- `makeAudioRow()`
- `makeAudioNavTile()`
- `buildAudioHome()`
- `buildAudioDecode()`
- `buildAudioOutputDetails()`
- `buildAudioOutputPolicy()`
- `buildAudioEq()`
- `buildAudioEqBand()`
- `buildAudioEffects()`
- `buildAudioDac()`

Settings entry changed from `音频/EQ` to `音频`, routing to `AudioHome`.

## Functional Boundary Kept

The WIP intentionally does not fake unsupported DSP/backend features.

Real controls still map to the existing first-version backend:

- 3-band tone:
  - low
  - mid
  - high
- balance
- preset mapping through existing `AudioToneSettings`
- persistence through existing `/settings.json` values:
  - `toneLP`
  - `toneBP`
  - `toneHP`
  - `balance`

Decode/output pages show live snapshot fields where available:

- codec
- sample rate
- bit depth
- bit rate

DAC page shows the confirmed hardware facts:

- DAC: `PCM5100A`
- I2S data: `GPIO7`
- I2S clocks: `BCLK15 / LRCK16`
- MUTE / AMP: not controlled, `-1`

Output policy / loudness / gapless / crossfeed are marked as not connected instead of being implemented as fake switches.

## Build / Verification Notes

Build attempts were interrupted by environment/runtime constraints, not by a remaining known compile error in the WIP UI file.

Observed sequence:

1. Direct `platformio` was not on PATH in the Codex shell.
2. Full PlatformIO path worked:
   - `C:\Users\tei_s\.platformio\penv\Scripts\pio.exe`
3. Building from the real repo path failed because the path contains a space:
   - `C:\Users\tei_s\Documents\esp32-s3 HIFIPlayer\...`
4. `scripts/build_windows.ps1 -UseSubst` with `Q:` hit a stale/default source scan issue looking for `rc\gbk_table.c`.
5. The script's junction path mode with `C:\mwr-src` compiled further and reached the WIP UI file.
6. One real compile error was found and fixed:
   - `lv_font_montserrat_22` was not available in this build config.
   - Replaced with existing `lv_font_montserrat_28`.
7. A later `-Jobs 4` build got past `src/ui/hifi_ui.cpp` and failed in `lib/tftLib/tft_spi.cpp` with compiler out-of-memory:
   - `cc1plus.exe: out of memory allocating 268439551 bytes`

Interpretation:

- The WIP UI source got past compilation in the later build.
- A full firmware success was not completed before pausing.
- Next machine should run a single-thread or low-parallelism build from a no-space path:
  - `.\scripts\build_windows.ps1 -Environment esp32s3 -BuildPath C:\mwr-src -Jobs 1`
  - or use the Mac/Linux equivalent if building at home.

## Suggested Next Steps At Home

1. Decide whether to keep this WIP direction or rebuild it more carefully from the design document.
2. Before editing further, inspect:
   - `src/ui/hifi_ui.h`
   - `src/ui/hifi_ui.cpp`
3. Complete a full build from a no-space path.
4. If build passes, flash and visually verify every page:
   - Settings -> Audio
   - Audio -> Decode / Output
   - Current Output
   - Output Policy
   - EQ / Effects
   - Single band adjustment
   - Effects
   - DAC / Amp
5. Only commit/push after visual verification on the 320x170 device.

