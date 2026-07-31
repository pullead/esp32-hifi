# Dev Log — 2026-07-31 night → 2026-08-01

Board: Waveshare ESP32-S3-Touch-LCD-1.9 (320x170, ST7789 SPI + CST816 touch).
Branch: `codex/radio-nowplaying-home-redesign`. Continues from
`docs/DEV_LOG_2026-07-30_home_redesign.md`.

This entry covers two unrelated pieces of work done back to back: a redesign
of the "当前输出" (audio output details) settings page, and a full debugging
saga to get the USB Mass Storage (drag-and-drop file transfer) feature —
started as a draft on the company machine via Codex on the
`codex/usb-storage-mount` branch — actually working end to end on real
hardware. The USB MSC section is long because nearly every step surfaced a
new, non-obvious failure; it's written in the order the failures were found
so the reasoning is reproducible if this ever needs revisiting.

## 1. "当前输出" page: spacing fix + per-metric icons

Complaint: the 6 metric cells (codec, sample rate in/out, bit depth,
channels, bitrate) had label/value text visibly overlapping in places, and
all 6 reused the same generic `LV_SYMBOL_AUDIO` glyph.

Root cause of the overlap: `lv_font_cjk_13`'s real line height is 17px, but
the old 48px-tall cell only budgeted 16px per stacked row (icon top,
label centered, value bottom via `LV_ALIGN_CENTER`/`LV_ALIGN_BOTTOM_MID`
fighting over the same vertical space) — same class of bug as the Home
Now-Playing label bug from the previous dev log entry.

Fix (`src/ui/hifi_ui.cpp`):
- Cell grew from 92×48 to 96×60, laid out with explicit non-overlapping
  y-offsets (icon at y=2, label at y=22, value at y=40) instead of
  center/bottom alignment.
- Added `buildAudioMetricIcon(lv_obj_t* parent, uint8_t kind)`: 6 small
  hand-drawn geometric icons (18×14, built from plain `lv_obj` rects/circles,
  no image assets — same visual language as the existing dot-matrix spectrum
  and VU ladder) — a ring for codec, mini waveforms (purple in / green out)
  for sample rate, an ascending staircase for bit depth, split L/R dots for
  channels, and 3 ascending color-coded bars for bitrate.
- Extracted `formatKhzCompact()` from `buildAudioDecode()`'s inline
  `"44k"`/`"44.1k"` formatting logic so `buildAudioOutputDetails()` could
  reuse it instead of duplicating the same snprintf branches.

## 2. USB MSC: from "点击完全没反应" to a mounted drive

Starting point: the company-machine Codex session had wired up
`USBMSC`/`onRead`/`onWrite`/SD `readRAW`/`writeRAW` plumbing in `main.cpp`,
gated behind `MWR_USB_MSC_SUPPORTED` (`SOC_USB_OTG_SUPPORTED &&
CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_MSC_ENABLED && !ARDUINO_USB_MODE`).
On real hardware: tapping "挂载" did nothing, no `[USBMSC]` log lines at all.

### 2.1 The dependency tree never had tinyusb in it
`sdkconfig.json` had no `CONFIG_TINYUSB_ENABLED` key at all — not "set to
false", *absent*, meaning the `custom_sdkconfig` lines setting it were being
silently dropped by kconfgen (unknown options are dropped, not errored on).
Traced through arduino-esp32's `CMakeLists.txt`: it only *links against*
tinyusb if `CONFIG_TINYUSB_ENABLED` is already true
(`if(...AND CONFIG_TINYUSB_ENABLED) maybe_add_component(arduino_tinyusb)`) —
it never adds the component itself. Nothing in this project's dependency
graph ever pulled tinyusb in, so the option genuinely didn't exist to set.

Fix: PlatformIO's espressif32 platform exposes `custom_component_add` (the
counterpart to the existing `custom_component_remove`) — added
`espressif/tinyusb` there. Confirmed via build log:
`[ComponentManager] Added component: espressif/tinyusb (*)`.

### 2.2 tusb_config.h: missing, then wrong include scope, then wrong scope again
Clean build now actually compiled tinyusb's class sources — and immediately
hit `fatal error: tusb_config.h: No such file or directory`. tinyusb ships
no config of its own; every consumer must supply one
(`managed_components/espressif__tinyusb/README.md` documents this exact
pattern: `target_include_directories(${tusb_lib} PRIVATE
path_to_your_tusb_config)` in the consumer's `CMakeLists.txt`).

- First attempt: added the snippet with `PRIVATE` scope — fixed tinyusb's
  *own* 4 class files (cdc/hid/midi/midi2 device), but arduino-esp32's
  `esp32-hal-tinyusb.c`/`USB.cpp` (a different component) hit the same
  "file not found" error, since `PRIVATE` doesn't propagate.
- Fixed by switching to `PUBLIC`: arduino-esp32's own `idf_component.yml`
  declares `espressif/tinyusb` as a manifest dependency, so the component
  manager wires a real `REQUIRES` edge from arduino-esp32 to tinyusb —
  `PUBLIC` rides that edge and reaches arduino-esp32's sources too.
- `src/CMakeLists.txt` also needed `PRIV_REQUIRES espressif__tinyusb` added
  to its own `idf_component_register()` call, so that
  `idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)`
  is guaranteed to run *after* tinyusb's own registration (ESP-IDF only
  supports querying a component's properties from something that already
  requires it).

### 2.3 Config content itself: several rounds of missing macros
Once the include path resolved, tinyusb's own `tusb_option.h` started
enforcing what it actually needs defined, one macro at a time:
- `CFG_TUSB_MCU` — already set via tinyusb's own `target_compile_options`,
  just needed an `#ifndef` guard instead of hardcoding it.
- `CFG_TUD_ENOINT0_SIZE` (sic — arduino-esp32's `esp32-hal-tinyusb.c:215`
  has a genuine upstream typo, missing the "P", inconsistent with its own
  `CFG_TUD_ENDOINT_SIZE` typo a few lines up in the header) — aliased to the
  correctly-spelled `CFG_TUD_ENDPOINT0_SIZE` rather than patching framework
  source.
- `CONFIG_TINYUSB_CDC_ENABLED` — arduino-esp32's `USB.h` declares
  `String`-typed descriptor fields (`product_name`, `manufacturer_name`,
  etc.) unconditionally once `CONFIG_TINYUSB_ENABLED`, but only pulls in
  `WString.h` via `USBCDC.h -> Stream.h -> Print.h -> WString.h`, and that
  chain is itself gated on `CONFIG_TINYUSB_CDC_ENABLED`. Without it:
  `'String' does not name a type` in `USB.cpp`/`USBCDC.cpp`/`HWCDC.cpp`.
  This project never actually uses USB CDC at runtime (`ARDUINO_USB_MODE=0`
  keeps `Serial` on UART) — the symbol only needs to be true at compile time
  to satisfy this header dependency.
- Turning CDC on then required `CFG_TUD_CDC` (tinyusb's own class enable,
  separate from arduino's `CONFIG_TINYUSB_CDC_ENABLED`) and
  `CONFIG_TINYUSB_CDC_RX_BUFSIZE` (referenced directly by name in
  `USBCDC.cpp`, not through the `CFG_TUD_` prefix).

None of `CONFIG_TINYUSB_ENABLED`/`_CDC_ENABLED`/`_MSC_ENABLED`/etc. are real
Kconfig symbols anywhere in this dependency tree by default — the
`arduino_tinyusb` component arduino-esp32's `CMakeLists.txt` references
doesn't exist in the ESP Component Registry (confirmed via its API:
`ComponentNotFoundError`); it only exists inside
`espressif/esp32-arduino-lib-builder`, used to build the *prebuilt*
`esp32-arduino-libs` that ship with the official Arduino IDE release. This
project builds arduino-esp32 from source instead, so that component (and its
Kconfig) never enters the picture.

Fix: `src/Kconfig.projbuild` (new file) declares the symbol family locally
— `TINYUSB_ENABLED`/`_CDC_ENABLED`/`_MSC_ENABLED`/`_HID_ENABLED`/
`_MIDI_ENABLED`/`_VENDOR_ENABLED`/`_DFU_ENABLED`/`_DFU_RT_ENABLED`, all
`default n`, all gated `depends on TINYUSB_ENABLED`. Symbol names and
menu structure are copied from the *actual* source of truth —
`esp32-arduino-lib-builder`'s own
`components/arduino_tinyusb/Kconfig.projbuild` — since arduino-esp32's C++
wrapper code was written against exactly those names. This works because
PlatformIO registers this project's `src/` as its own ESP-IDF component
(see `espidf.py`'s `EXTRA_COMPONENT_DIRS` handling), and ESP-IDF's Kconfig
collection walks every component root for a `Kconfig.projbuild` file
regardless of whether that component's `CMakeLists.txt` is hand-written —
so declaring the symbols there makes `platformio.ini`'s existing
`custom_sdkconfig` lines finally bind to something real.

`src/tusb_config.h` (new file) mirrors — for the classes this project
compiles via the managed `espressif/tinyusb` component (which, unlike
`arduino_tinyusb`, only builds the *device* stack, no host/mtp/printer/bth/
usbtmc/ecm_rndis) — `esp32-arduino-lib-builder`'s own
`components/arduino_tinyusb/include/tusb_config.h`: `CFG_TUD_X` mapped from
`CONFIG_TINYUSB_X_ENABLED`, everything not compiled by our component
hardcoded off.

Only `TINYUSB_ENABLED`/`_CDC_ENABLED`/`_MSC_ENABLED` are actually turned on
in `platformio.ini`'s `custom_sdkconfig`; HID/MIDI/vendor/DFU/DFU-runtime
stay off (CDC is compiled in only for the `String` header dependency above,
never instantiated at runtime). Note: an earlier build cycle briefly had a
fuller canonical Kconfig with several of those defaulting to `y`, which
left stray `CONFIG_TINYUSB_HID_ENABLED=y` (etc.) lines cached in the
tracked `sdkconfig.esp32s3_OTA` file even after the defaults were trimmed
back to `n` — PlatformIO's sdkconfig merge preserves values from the
existing file rather than resetting to the current Kconfig default, so
those had to be manually cleaned out of the tracked file too.

### 2.4 Bootloader: never gets built when `custom_sdkconfig` is used
Full clean build finally succeeded (`firmware.bin` generated) — but
`pio run -t upload` (over serial, forced via `PLATFORMIO_UPLOAD_PROTOCOL`
since this env's default `upload_protocol` is `espota`, itself vestigial —
see the `[env:esp32s3_OTA]` comment, this board never used network OTA)
failed: `esptool write-flash` couldn't find `bootloader.bin`.

This is the *exact same* problem already diagnosed and solved in
`docs/DEV_LOG_2026-07-28_evening.md` §1: PlatformIO's `espidf.py` has its
own `build_bootloader()` SCons path, invoked via
`if flag_custom_sdkonfig == False: env.Depends(...)` — since this project
uses `custom_sdkconfig`, bootloader building is unconditionally skipped, on
every build, forever. The fix from that entry still applies verbatim: use
the already-committed, pre-verified `firmware/bootloader_dio_40m_esp32s3.bin`
(built once via a standalone bootloader-only PlatformIO project using
`sdkconfig.defaults` instead of `custom_sdkconfig`) rather than trying to get
PlatformIO to build one. Flashed via `esptool write-flash` directly:
`bootloader_dio_40m_esp32s3.bin@0x0` + `partitions.bin@0x8000` +
`firmware.bin@0x10000`. (Only needed once — the bootloader and partition
table haven't changed since 7/28, so later iterations only re-flash
`firmware.bin@0x10000`.)

### 2.5 Single USB-C port: console and MSC can't coexist
This board has exactly one USB-C connector. Before `USB.begin()` runs, it
behaves as a normal serial console (`/dev/cu.usbmodem*`, 921600 baud, per
existing project convention). The instant `usbStorageMountTask()` calls
`USB.begin()` to bring up the native OTG/TinyUSB device stack, the physical
port re-enumerates as the MSC device instead — any open serial connection
dies immediately (`OSError: [Errno 6] Device not configured` on the host
side), and it does **not** come back until the board is power-cycled
(`s_usbStarted` is set once and never unset, so even unmounting MSC and
calling `s_usbMsc.end()` doesn't release the native USB peripheral back to
console mode). This makes normal "watch the log scroll" debugging of
anything past that point impossible — confirmed by testing, not assumed.

Workaround used for the rest of this session: print whatever's needed
*before* `USB.begin()` runs (still on the console-capable side of the
transition), then power-cycle between every firmware iteration to get the
console back for the next round.

### 2.6 The actual bug: `SD_MMC.numSectors()` lies about capacity
With console access gone past the mount point, macOS's own signal became
the only feedback: tapping "挂载" did make a USB Mass Storage device
enumerate (real progress — earlier failures never got a device on the bus at
all), but Finder/Disk Utility immediately showed **"此电脑不能读取你连接的
磁盘"** ("this computer can't read the disk you connected").

Added a diagnostic dump (before `USB.begin()`, per §2.5) of the raw LBA0
sector — printed the BPB region, the MBR partition table (bytes 446-509),
and the boot signature. Result: valid `55 AA` boot signature, and a real,
sane-looking MBR partition entry — type `0x07` (exFAT/NTFS), starting at
LBA 63, spanning 125,173,697 sectors (end LBA 125,173,759). But the total
sector count this firmware was reporting to the USB host was only
123,217,825 — **the partition table describes a partition ending about
1,955,935 sectors (~955MB) past the end of what the exposed block device
claims to have**. Any host filesystem driver sanity-checking partition
bounds against reported disk size would reject that outright — exactly
matching the symptom.

Root cause: `usbStorageMountTask()` used `SD_MMC.numSectors()` for the
sector count passed to `s_usbMsc.begin()`. Despite the name, that function
(`SD_MMC.cpp`) is **not** "total sectors on the card" — it's
`totalBytes() / sectorSize()`, where `totalBytes()` comes from `f_getfree()`
cluster accounting on the *currently mounted FAT filesystem* — i.e. the
filesystem's usable *data area*, excluding the boot sector, reserved
sectors, FAT tables, and root directory region. That's inherently smaller
than the card's real physical capacity, by whatever the filesystem's own
metadata overhead is (here, coincidentally close to the size of the
partition offset + FAT/exFAT metadata for a ~64GB card). `SD_MMC.cardSize()`
(`_card->csd.capacity * _card->csd.sector_size`) is the actual physical
capacity and is what a raw block-device passthrough needs.

Fix: `sectorCount` now computed as `SD_MMC.cardSize() / SD_MMC.sectorSize()`
instead of `SD_MMC.numSectors()`.

**Status at end of session: fix applied, rebuilt, reflashed — not yet
re-verified on hardware** (a fresh mount attempt after this exact fix still
showed the same "无法读取" dialog once; a log capture of that attempt was
lost to a race in the test harness rather than confirming the fix failed —
needs a clean re-test with the diagnostic dump still in place to confirm the
corrected `sectorCount` actually resolves it before this can be marked
done).

## Files touched
- `src/ui/hifi_ui.cpp` — audio output page spacing + per-metric icons,
  `formatKhzCompact()` extraction.
- `platformio.ini` — `custom_component_add = espressif/tinyusb`;
  `CONFIG_TINYUSB_CDC_ENABLED=y` added alongside the existing `_ENABLED`/
  `_MSC_ENABLED` lines, all 3 envs.
- `src/CMakeLists.txt` — `PRIV_REQUIRES espressif__tinyusb` +
  `target_include_directories(tusb_lib PUBLIC ...)` for `tusb_config.h`
  visibility.
- `src/Kconfig.projbuild` (new) — project-local `CONFIG_TINYUSB_*` symbol
  declarations.
- `src/tusb_config.h` (new) — tinyusb device-stack configuration.
- `sdkconfig.esp32s3_OTA` — synced TinyUSB section, stray HID/MIDI/vendor/
  DFU `=y` leftovers cleaned back to their Kconfig defaults (`n`).
- `src/main.cpp` — `usbStorageMountTask()`: `cardSize()`-based sector count
  instead of `numSectors()`; pre-`USB.begin()` diagnostic LBA0 dump.
