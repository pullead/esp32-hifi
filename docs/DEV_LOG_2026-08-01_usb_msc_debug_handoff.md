# DEV LOG 2026-08-01 USB MSC Debug Handoff

## Branch

- New handoff branch: `codex/usb-msc-handoff-20260801`
- Previous working branch: `codex/usb-msc-capacity-fix`
- Purpose: preserve today's company-side USB mass-storage debugging work for the home machine.

## Scope

Today's work focused only on the Settings USB storage mount feature:

- Export the onboard SD card to Windows as a USB mass-storage disk.
- Keep normal USB serial behavior outside the test path.
- Diagnose why Windows can see the drive letter but cannot open the volume.

No EQ, audio playback, WiFi, font, or Now Playing UI changes were intentionally made today.

## What Changed

### USB MSC Diagnostics

- Added USB MSC read/write counters and status values in `src/main.cpp`.
- Exposed these counters through `PlayerService::usbStorageStats()`.
- Added a compact debug label in the USB storage settings panel.
- The UI debug line now reports:
  - read request count and read failure count
  - write request count and write failure count
  - last transfer size
  - last callback return value
  - last/min/max LBA
  - max host request size

### Disk Geometry Probing

- Added MBR parsing and validation for SD card LBA0.
- Added support for testing both full-disk exposure and partition-only exposure.
- Current test setting is full-disk exposure:
  - `kUsbMscExposePartitionOnly = false`
  - host sees the whole SD card, including MBR

### Boot-Time USB MSC Probe

- Runtime `USB.begin()` after app startup caused reset/reboot.
- Current test setting initializes TinyUSB at boot:
  - `kUsbMscBootPresentProbe = true`
  - media is presented at boot for debugging

This is not the final product behavior. It is a test mode to isolate USB enumeration and Windows volume mounting.

### TinyUSB MSC Buffer

- `src/tusb_config.h` previously hardcoded:
  - `CFG_TUD_MSC_EP_BUFSIZE 512`
- Changed it to use:
  - `CONFIG_TINYUSB_MSC_BUFSIZE`
- Added fallback:
  - `CONFIG_TINYUSB_MSC_BUFSIZE 4096`

Reason: `sdkconfig.defaults` already requests 4096, but the generated build did not expose that symbol to this custom `tusb_config.h`.

## Verified Today

### Build

PlatformIO build succeeded after the TinyUSB buffer fallback:

- Environment: `esp32s3_OTA`
- RAM: about `29.0%`
- Flash: about `30.5%`
- Firmware was generated at:
  - `.pio/build/esp32s3_OTA/firmware.bin`

### Flash

App-only flash succeeded over `COM5`:

- Chip: ESP32-S3
- Flash offset: `0x10000`
- Bootloader and partition table were not reflashed.

### Windows Disk Detection

With full-disk mode enabled, Windows detects the SD card geometry correctly:

- Friendly name: `ESP32S3 HiFi SD`
- Bus type: USB
- Partition style: MBR
- Size: `64088965120`
- Partition:
  - Drive letter: `E`
  - Type: `IFS`
  - MBR type: `7`
  - Offset: `32256`
  - Size: `64088932864`

This confirms the earlier partition-only mode was wrong for Windows.

## Current Problem

The drive letter appears, but Windows still cannot open the volume.

Observed behavior:

- Explorer shows `USB Drive (E:)`.
- Opening `E:` spins indefinitely.
- `Get-Volume -DriveLetter E` times out.
- `Get-ChildItem E:\` previously also timed out.
- Firmware-side read failures remain `0`.
- Firmware-side write count remains `0`.
- Windows keeps issuing read requests.

Before the 4096 buffer build, UI reported examples like:

```text
R.../0 W0/0 S512 R512
L... 0-... O0 M512
```

After the 4096 buffer build, the next machine should confirm whether the debug label reports `M4096`.

## Current Diagnosis

The USB device-level and disk partition-level pieces are mostly working:

- TinyUSB enumerates as a USB disk.
- Windows sees the expected whole-card MBR.
- Windows assigns drive letter `E`.
- The firmware is returning successful read callbacks.

The remaining failure is at filesystem-volume mounting time.

Most likely areas still needing investigation:

1. MSC transfer size or throughput is still too small/slow for Windows volume probing.
2. Arduino `SD_MMC.readRAW()` only performs single-sector reads through FatFS `disk_read(..., 1)`.
3. Windows may be reading exFAT/FAT metadata sector-by-sector and timing out.
4. There may still be a subtle read consistency issue under repeated random LBA access.

## Important Gotchas

- Do not leave `kUsbMscBootPresentProbe = true` as final behavior.
- Final behavior should return to "normal app by default, USB disk only when user enables it".
- The current branch is a debugging handoff, not a stable feature branch.
- Do not test with the only copy of important SD card data.
- Always eject the USB drive on Windows/macOS before disabling mount mode.

## Suggested Next Steps

1. Flash this branch on the home machine.
2. Confirm the UI debug line after Windows tries opening `E:`.
3. Specifically check whether max transfer size changed:
   - expected after this handoff: `M4096`
   - if still `M512`, TinyUSB config is still not applied to the compiled MSC component
4. If `M4096` appears but the drive still hangs, add a protected-access wrapper around `SDMMCFS` to call FatFS `disk_read(_pdrv, buffer, lba, count)` for multi-sector reads.
5. Add focused logging for the first 100 read requests:
   - LBA
   - offset
   - size
   - return value
6. Compare those reads against a known-good raw SD dump if needed.
7. After the disk opens reliably, revert boot-present debug mode and redesign the feature as a controlled "enter USB disk mode" flow.
