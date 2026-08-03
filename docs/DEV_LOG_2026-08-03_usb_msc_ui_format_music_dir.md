# DEV LOG 2026-08-03: USB MSC UI, FAT32/32KB, and Local Music Folder

## Branch

- Working branch: `codex/usb-msc-ui-format-polish`
- Base context: continued from `codex/usb-msc-handoff-20260801`

## Goals

1. Diagnose why Windows USB MSC drive appeared but took a very long time to open.
2. Restore normal startup behavior: the board should stay in normal serial/player mode until the user taps USB mount.
3. Replace the rough USB mount page with a clearer 320x170 layout.
4. Add a dedicated SD card folder for local music and lyrics.

## SD Card Format Finding

Windows `Get-Volume E:` showed the SD card was FAT32 with a 512-byte allocation unit. On a 64 GB FAT32 volume this produces a very large cluster count and huge FAT metadata, which explained the slow first open behavior over USB MSC.

The card was backed up, reformatted to FAT32 with 32 KB allocation units, then restored. After this, Windows opened the USB MSC drive in a few seconds instead of taking many minutes.

Backup path kept locally and intentionally not committed:

```text
backups/sd_card_before_fat32_32k_20260803_113501
```

## Code Changes

### USB MSC Startup

Changed the debug boot probe mode back to normal behavior:

```cpp
static constexpr bool kUsbMscBootPresentProbe = false;
```

Result: plugging the board into the computer no longer automatically exposes the SD card as a USB disk. The SD card is exposed only after tapping the UI mount action.

### USB Storage Format Info

Added `UsbStorageFormatInfo` and a `PlayerService::usbStorageFormatInfo()` API so the UI can display:

- FAT type
- allocation unit size
- recommended FAT32/32 KB state
- used/total capacity

The information is parsed from the FAT BPB at the first partition start LBA.

### USB Mount UI

Reworked the USB mount page into a compact dark UI:

- Top bar: `U盘挂载`
- Main state panel with USB icon, status, detail, and hint
- Format box, e.g. `FAT32/32KB`
- Capacity box, e.g. `39MB/59.7GB`
- Single primary action button: mount / unmount / wait state

The dynamic Chinese text uses `HIFI_FONT_DYNAMIC_TEXT` instead of `lv_font_cjk_16`, because `lv_font_cjk_16` is intentionally a limited title subset and caused tofu/garbled text for new dynamic strings.

### Dedicated Local Music Folder

Added a fixed local music directory:

```cpp
static constexpr const char* kLocalMusicDir = "/Music";
```

The firmware now ensures `/Music` exists:

- after SD initialization during LVGL runtime setup
- before local library scanning
- after exiting USB MSC mode and remounting the SD card

The local library still scans from `/`, so existing root-level MP3 files remain compatible. New music and lyrics should be placed under:

```text
/Music/song.mp3
/Music/song.lrc
```

Sidecar `.lrc` behavior is unchanged: lyrics are derived from the MP3 path with the same basename and `.lrc` extension, so MP3 and LRC should live in the same folder.

## On-Device Verification

Verified on the Waveshare ESP32-S3-Touch-LCD-1.9 board:

- Normal USB connection no longer defaults to a mounted SD disk.
- USB mount page Chinese text no longer shows garbled/tofu characters.
- Tapping USB mount exposes the SD card to Windows.
- FAT32/32 KB formatted card opens quickly.
- `/Music` folder is created and visible from Windows after USB mount.

## Build Verification

PlatformIO build:

```text
platformio run -e esp32s3_OTA -j 4
```

Result:

```text
SUCCESS
RAM:   29.0% (95132 / 327680 bytes)
Flash: 30.5% (5114170 / 16777216 bytes)
```

Flashing the app image to `0x10000` completed with:

```text
Hash of data verified.
```

## Notes for Next Machine

- Do not commit `backups/`; it contains the SD card backup from before reformatting.
- `usb_msc_serial_capture.log` is a temporary debug artifact and was not committed.
- If future work wants the board to enumerate only as USB-Serial/JTAG until mount is tapped, the next deeper change is to delay TinyUSB MSC initialization itself. Current fixed behavior is: no media is presented at boot, and practical user behavior is correct.
- Embedded MP3 cover art is supported for JPG/JPEG APIC/PIC frames. Lyrics are most reliable as same-folder `.lrc` sidecars; embedded lyrics may need future USLT/SYLT expansion depending on the downloader's tag format.
