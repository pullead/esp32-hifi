# USB MSC SD Card Development Notes - 2026-08-01

## Goal

Implement a Settings entry that lets the user expose the ESP32-S3 board's SD card to a PC or Mac as a USB mass-storage drive.

Target final behavior:

- Normal USB connection by default.
- User taps Settings > USB storage mount.
- Playback stops.
- App gives exclusive SD-card ownership to USB MSC.
- Computer shows the SD card as a removable disk.
- User copies, deletes, or moves music files.
- User ejects the disk on the computer.
- User disables mount mode.
- Firmware remounts SD and rescans local music.

## Current State

Current handoff branch is still a debugging branch.

It can enumerate a USB disk and Windows can assign a drive letter, but Windows cannot complete filesystem volume mounting yet.

The current test build intentionally presents MSC media at boot to avoid the reset caused by runtime `USB.begin()`.

## Architecture Added

### Core State

`UsbStorageState` is exposed through PlayerService:

- `Idle`
- `Mounting`
- `Mounted`
- `Restoring`
- `Scanning`
- `Error`
- `Unsupported`

### UI Boundary

The UI calls PlayerService methods instead of touching USB/SD internals directly:

- `usbStorageState()`
- `usbStorageMount()`
- `usbStorageUnmount()`
- `usbStorageStats()`

### Storage Boundary

The backend currently uses Arduino-ESP32:

- `USBMSC`
- `SD_MMC.readRAW()`
- `SD_MMC.writeRAW()`

The callbacks translate host LBA requests into SD raw-sector reads/writes.

## Known Working Findings

### Runtime USB.begin Is Not Safe

Calling `USB.begin()` when the app is already running caused a reset before the next log line.

Evidence from earlier serial logs:

```text
[USBMSC] before USB.begin resetReason=11
```

No `after USB.begin` log was reached.

Current debug workaround: initialize TinyUSB at boot.

### Partition-Only Exposure Is Wrong

Partition-only mode made Windows misinterpret the 64GB volume:

- Windows reported `FAT16`
- MBR type looked wrong
- Offset was `0`

This path should not be used for the current SD card.

### Full-Disk Exposure Is Correct

Full-disk exposure gives Windows a correct MBR view:

```text
Disk: ESP32S3 HiFi SD
PartitionStyle: MBR
Size: 64088965120
Partition: E:
MbrType: 7
Offset: 32256
Size: 64088932864
```

This is the correct base for further debugging.

## Current Hypothesis

The remaining problem is not USB enumeration or partition geometry.

The failure happens when Windows tries to mount/read the filesystem volume.

The best current hypothesis is transfer efficiency or consistency:

- `CFG_TUD_MSC_EP_BUFSIZE` was hardcoded to `512`.
- TinyUSB caps MSC transfers to this buffer size.
- Windows may issue a large number of metadata reads for the large SD filesystem.
- Arduino `SD_MMC.readRAW()` itself only reads one sector at a time.

The current branch changes TinyUSB MSC buffer selection to use a 4096-byte fallback. The next test should verify whether the UI debug field changes from `M512` to `M4096`.

## Files Changed

- `src/main.cpp`
  - USB MSC setup and callbacks
  - disk geometry detection
  - boot-present debug path
  - read/write diagnostics
  - mount/unmount state handling
- `src/tusb_config.h`
  - MSC endpoint buffer no longer hardcoded to 512
  - fallback value set to 4096
- `src/ui/player_service.h`
  - USB storage state/stat interfaces
- `src/ui/player_service.cpp`
  - bridge to core USB MSC stats
- `src/ui/hifi_ui.h`
  - USB debug label field
- `src/ui/hifi_ui.cpp`
  - USB storage settings panel diagnostics

## Build and Flash Notes

On the company Windows machine, PlatformIO is not on PATH. Working commands:

```powershell
subst P: "C:\Users\tei_s\Documents\esp32-s3 HIFIPlayer"
cd P:\ESP32-MiniWebRadio-src
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e esp32s3_OTA -j 4
```

App-only flash:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM5 --baud 921600 --before default_reset --after hard_reset write_flash 0x10000 ".pio\build\esp32s3_OTA\firmware.bin"
```

When the app is enumerated as USB MSC, the serial port disappears. To enter bootloader manually:

1. Close any Explorer window opened on the USB drive.
2. Unplug USB.
3. Hold `BOOT`.
4. Plug USB back in.
5. Keep holding for about 5 seconds.
6. Release `BOOT`.

## Recommended Next Implementation Step

If `M4096` is confirmed but the volume still cannot open, implement multi-sector raw reads/writes.

Reason:

- Arduino `SD_MMC.readRAW()` calls FatFS `disk_read(_pdrv, buffer, sector, 1)`.
- The underlying FatFS disk API supports a sector count.
- `SDMMCFS::_pdrv` is protected, so the cleanest local experiment is a small derived helper that exposes:
  - `readSectors(buffer, lba, count)`
  - `writeSectors(buffer, lba, count)`

This should be done as a contained experiment and verified with:

- `M4096` on UI
- read failure count remains `0`
- Windows `Get-Volume -DriveLetter E` returns quickly
- Explorer can list files
- copy a small file to SD
- safely eject
- firmware remounts SD and rescans local library

## Final Product Direction

After the storage layer is reliable, replace boot-present debug mode with a user-facing flow:

- Tap mount.
- Show a confirmation that playback will stop.
- Persist a temporary "enter USB storage mode" flag.
- Reboot into USB storage mode if runtime `USB.begin()` remains unsafe.
- In USB storage mode, keep UI minimal and do not run playback/SD scanning.
- Tap unmount or host eject returns to normal app mode and rescans SD.
