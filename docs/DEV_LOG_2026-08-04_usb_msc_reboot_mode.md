# DEV LOG 2026-08-04: Pause/Play Fix, Phase A Wrap-Up, USB MSC Reboot-Based Storage Mode (Phase B)

## Branch

- Handoff branch (new): `codex/usb-msc-reboot-mode` -- carries today's Phase B
  changes plus this log, pushed for the home machine.
- Base: `codex/usb-msc-ui-format-polish` at `d01b5be` (today's two commits
  below were already pushed there earlier today).
- This is a handoff to Claude Code on the home computer: read this log first,
  then verify the on-device chain listed in "Pending verification" below.

## Today's commits (already on `codex/usb-msc-ui-format-polish`)

### `2709318` fix: correct pause/resume state tracking after `Audio::pauseResume()`

Root cause: `Audio::pauseResume()` returns whether the toggle was *accepted*
(an audio source is active), not whether playback is now paused. The old code
latched `s_f_pauseResume` to that return value, so the second press (resume)
still left the latch `true` and `PlayerSnapshot` always reported `Paused` --
the UI could never switch back to playing.

Fix: centralized `audioPauseResumeAndUpdateState()` derives the real state
from `!audio.isRunning()` only when the toggle was accepted. All call sites
(terminal `pr` command, IR short-key 18, web `pause_resume`, PL/DL GUI
buttons, `PlayerService::togglePause`) route through it; the latch is reset in
`connecttohost()`/`connecttoFS()`.

### `d01b5be` feat: resume last local track position + explicit source-switch playback

- `lastconnectedfilepos` persisted in `/settings.json`; the Local Now Playing
  play button resumes the last local file from its remembered position.
- Radio page play button starts the current station; next/prev station stop
  SD playback first.
- **User decision (B):** navigating between pages never stops playback -- only
  an actual play action (play button OR prev/next) switches the source. The
  stop-on-navigation block in `HifiUi::show()` was removed; the mutual
  exclusion lives in `playRadioUrl()`/`playRadioStation()`/`playSdFile()` and
  `nextStation()`/`previousStation()`.
- Build fix: `onLocalTransportAction()` is static, so instance members now go
  through `s_instance->` (the uncommitted morning tree did not compile).
- `.gitignore`: added `backups/` and `usb_msc_serial_capture.log` (local
  board-flash/SD backups may contain credentials -- never commit).

## Full flash backup (before flashing anything new)

- `backups/board_flash_20260804_130848/` -- 16 MB full dump + bootloader +
  partition table + factory partition, with `ANALYSIS.md` (SHA256s, image
  info, comparison).
- The board's app partition was byte-identical to the local
  `.pio/build/esp32s3_OTA/firmware.bin` (mtime 09:59, this morning's build),
  even though the embedded app-desc metadata still read `da253b5-dirty /
  Aug 1 12:12:47` -- incremental builds don't regenerate `esp_app_desc`, so
  the byte comparison, not the metadata, is authoritative.

## Root cause found: "after flashing, I must press the reset button once"

`usbStoragePrepareMsc()` unconditionally called `USB.begin()` at **every**
boot, even with `kUsbMscBootPresentProbe = false` (that flag only controls
whether media is exposed, not whether TinyUSB starts). TinyUSB then took over
the board's single USB-C ~2 s after boot, killing the USB-Serial/JTAG console
and making the board look like it never returned to normal mode. This started
07-31 when TinyUSB was wired in.

Also found: the active compact USB page had the M4096 debug line as **dead
code** -- `m_usbStorageDebug` was only created/updated in the unused second
layout variant, so the UI never displayed it.

## Phase B: USB MSC reboot-based storage mode (uncommitted here, on the new branch)

### Design decision (approved)

Runtime `USB.begin()` is unsafe on this board (documented ESP_RST_USB,
resetReason=11); boot-time TinyUSB init is the only verified-stable point. So
the final product flow is:

1. Normal mode: **never** start TinyUSB at boot (serial console stays alive).
2. Tap mount (with a two-tap confirmation) -> stop playback -> write NVS flag
   `usb_msc_mode` -> reboot.
3. Storage mode (flag set at boot): init TinyUSB with media present, minimal
   UI (USB page only), no WiFi/audio/scan.
4. Host eject (or the unmount button) -> clear flag -> reboot back to normal
   mode -> SD remount + `/Music` rescan.

### Changes

`src/main.cpp`:
- NVS helpers `usbMscModeFlag()` / `usbMscSetModeFlag()` (Preferences key
  `usb_msc_mode`, guarded by `s_prefMutex`).
- `usbStorageProbeGeometry()` extracted from `usbStoragePrepareMsc()` -- pure
  SD geometry/format probe with no USB side effects; called at every normal
  boot so the USB page still shows FAT/allocation/capacity.
- `setupLvglRuntime()`: if the flag is set -> `usbStoragePrepareMsc(true)` +
  `lvglRuntimeBegin()` + `lvglRuntimeShowUsbStoragePage()`, skipping
  WiFi/audio/scan; on init failure the flag is cleared and the board reboots
  to normal mode. Normal path no longer touches TinyUSB.
- `loopLvglRuntime()`: storage-mode shortcut (only `processUsbStorage()` +
  `lvglRuntimeTick()`), so no audio/web/dlna ticks run in storage mode.
- Old runtime mount/unmount tasks replaced by
  `usbStorageRebootToMscTask()` / `usbStorageRebootToNormalTask()` (flag +
  `ESP.restart()`, with a short quiesce wait on eject).
- `playerCoreUsbStorageMount()`: `stopSong()` + set flag + reboot task;
  `playerCoreUsbStorageUnmount()`: clear flag + reboot task.

`src/ui/hifi_ui.{h,cpp}`:
- Two-tap mount confirmation: first tap arms the button ("再次点击确认挂载",
  5 s expiry), second tap mounts.
- M4096 debug line re-added to the ACTIVE compact USB page (panel 72 -> 86 px,
  four-line layout, stats format `R.. W.. S.. R.. M..`).
- `HifiUi::showUsbStoragePage()` static helper.

`src/ui/lvgl_runtime.{h,cpp}`:
- `lvglRuntimeShowUsbStoragePage()` bridge.

### Build / flash / verification so far

- Build: SUCCESS. RAM 29.0% (95,148 / 327,680, +16 B), Flash 30.5%
  (5,116,646 B, +732 B). `firmware.bin` 5,117,040 B.
- Flashed to the board (app partition only). After the boot, **COM5 stayed
  alive** -- the old firmware killed it within ~2 s. So the "manual reset
  after flashing" symptom is fixed at the root.
- Full serial boot-log capture did not work from this machine's session (the
  app's `printf` routing to the USB-Serial/JTAG console is unclear; the UI is
  the reliable check).

### Pending verification (home machine, on-device)

1. Settings -> USB storage page shows FAT32/32KB, capacity, and the debug
   line -- confirm **M4096** (`CFG_TUD_MSC_EP_BUFSIZE = 4096` effective).
2. Tap mount -> button becomes "再次点击确认挂载" -> tap again -> playback
   stops, board reboots into storage mode.
3. Windows shows the SD drive and opens it quickly; copy a small file.
4. Safe-eject in Windows -> board auto-reboots to normal mode -> SD remount +
   `/Music` rescan (no duplicate/lost tracks, `/Music` intact).
5. If M4096 is confirmed but Windows is still slow/hangs: implement
   multi-sector raw reads (SDMMCFS-derived helper calling FatFS
   `disk_read(..., count)`) -- the only remaining performance suspect.

## Handoff notes for the home machine (Claude Code)

- Working branch: `codex/usb-msc-reboot-mode` (this log + Phase B changes are
  pushed there). `codex/usb-msc-ui-format-polish` already has today's two
  commits.
- Build/flash as usual: `scripts/build_windows.ps1`, flash
  `.pio/build/esp32s3_OTA/firmware.bin` @ `0x10000`.
- Storage-mode NVS flag persists across power cycles (until eject/unmount) --
  intended.
- Do not commit `backups/` or `pio-*.log` (gitignored).
- Known build quirk: a PlatformIO run can re-trigger CMake reconfigure and
  regenerate `sdkconfig.h`, invalidating all ESP-IDF components -> one full
  rebuild (~1 h). A full rebuild was completed today at 16:19, so incremental
  builds should be fast again; if a full rebuild happens, it is expected, not
  a hang. `-j1` stays the safe default on Windows.
- Board state at handoff: running the new Phase B firmware; the on-device
  chain test above was not yet completed.
