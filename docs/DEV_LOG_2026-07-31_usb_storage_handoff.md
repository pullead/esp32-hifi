# DEV LOG 2026-07-31 USB Storage Handoff

## Branch

- Branch: `codex/usb-storage-mount`
- Base branch before split: `codex/audio-settings-on-radio-home`
- Purpose: preserve today's company-side work for handoff to the home machine.

## Completed Today

### Audio Settings Polish

- Refined the audio settings title-bar status badge.
- `解码与输出` title bar now shows the current codec/sample-rate summary as a compact badge with an audio icon.
- `EQ 与音效` title bar now shows the EQ state as a green `ON` badge.
- EQ overview layout was changed to four compact modules:
  - `低音`
  - `中音`
  - `高音`
  - `平衡`
- Each EQ module keeps the slider on the left and text/value on the right, reducing the earlier visual clutter.
- Gesture mis-click suppression from the previous work remains in place.

### USB Mass Storage First Draft

- Added a planned `设置 > U盘` entry.
- Added `UsbStorageState` to the PlayerService boundary:
  - `Idle`
  - `Mounting`
  - `Mounted`
  - `Restoring`
  - `Scanning`
  - `Error`
  - `Unsupported`
- Added PlayerService methods:
  - `usbStorageState()`
  - `usbStorageMount()`
  - `usbStorageUnmount()`
- Added a first-draft USB MSC backend in `src/main.cpp`:
  - uses Arduino-ESP32 `USBMSC`
  - uses `SD_MMC.readRAW()` / `SD_MMC.writeRAW()`
  - stops playback before mounting
  - blocks local SD playback while USB storage is active
  - pauses web/FTP loop handling while SD is exposed over USB
  - handles host eject through the MSC start/stop callback
  - unmount path hides MSC media, remounts SD, and starts a local-library rescan
- Converted the local music scanner into a reusable `startLocalMusicScan()` entry:
  - clears old in-memory library before rescan
  - restores the scanning flag on task creation failure
  - can be triggered after USB storage mode exits
- Added `platformio.ini` config for TinyUSB MSC:
  - `CONFIG_TINYUSB_ENABLED=y`
  - `CONFIG_TINYUSB_MSC_ENABLED=y`
  - `ARDUINO_USB_MODE=0`
  - `ARDUINO_USB_MSC_ON_BOOT=0`

## Current Status

- USB storage code is not yet verified.
- A full build was started after the USB changes.
- The first build attempt with `Jobs 4` stopped making progress: several `xtensa` compiler processes remained alive but CPU time and output files stopped advancing.
- Those stalled processes were terminated.
- A single-thread rebuild was started but interrupted intentionally when we paused for handoff, so there is no confirmed `firmware.bin` from the USB-storage code yet.

## Next Steps On Home Machine

1. Pull branch `codex/usb-storage-mount`.
2. Run a clean single-thread build first:
   - `scripts/build_windows.ps1 -Environment esp32s3 -BuildPath C:\mwr-src -Jobs 1`
3. If TinyUSB symbols fail to compile, inspect generated `sdkconfig.h` for:
   - `CONFIG_TINYUSB_ENABLED`
   - `CONFIG_TINYUSB_MSC_ENABLED`
   - `ARDUINO_USB_MODE`
4. After build success, flash app firmware and test:
   - connect board to computer
   - confirm no SD card appears by default
   - open `设置 > U盘`
   - tap mount
   - confirm computer shows SD card
   - copy/delete a small test file first
   - safely eject on computer
   - confirm device enters restoring/scanning state
   - confirm local music list updates after scan
5. Do not test with important SD contents first. Use a backed-up card or copy a few non-critical files for the first MSC test.

## Known Risk

- USB MSC exposes the SD card as a block device. If Windows/macOS still has write cache pending and the device is forced to end mount mode, FAT corruption is possible.
- The UI text intentionally recommends safe eject on the computer before tapping `结束挂载`.
- FTP/web SD access is paused while USB storage is active, but this first draft still needs hardware verification.

