# ESP32-MiniWebRadio Port Analysis

## A. Repository Structure

Local source: `ESP32-MiniWebRadio-src/`

Important paths:

- `platformio.ini`: PlatformIO environments, framework versions, build flags, library dependency on `ESP32-audioI2S`.
- `src/settings.h`: board/display/GPIO configuration is currently centralized here.
- `src/main.cpp`: setup flow, I2S initialization, SD_MMC mount, display/touch initialization, web/UI event handling.
- `src/common.h`: display and touch class selection based on `TFT_CONTROLLER` and `TP_CONTROLLER`.
- `src/mwr_src/layout.hpp`: fixed-coordinate UI layout objects.
- `src/mwr_src/graphical.hpp`: UI drawing and SD image access.
- `src/mwr_src/function.hpp`: station list, playlist, SD helpers, metadata handling.
- `lib/tftLib/`: display and touch drivers.
- `boards/*.csv`: partition tables.
- `Content_on_SD_Card.zip`: required SD card resources.

## B. Build Environment

- Framework: `arduino, espidf`, meaning Arduino as an ESP-IDF component under PlatformIO.
- Platform: `pioarduino/platform-espressif32` release `55.03.39`.
- Commented version: Arduino 3.3.9, ESP-IDF 5.5.4.
- Default ESP32-S3 environment: `[env:esp32s3]`.
- Monitor speed: `921600`.
- Upload speed: `1500000`.
- Audio task core flag: `AUDIOTASK_CORE=0`.
- Arduino setup/loop core flag: `ARDUINO_RUNNING_CORE=1`.
- PSRAM flag: `BOARD_HAS_PSRAM=1`.

Current default board in upstream:

- `ESP32-S3-DevKitC-1-N4R8`
- Current active ESP32-S3 partition table is `boards/miniwebradio4MB.csv`, while the target board needs a 16 MB profile.

## C. Hardware Abstraction

The upstream project does not have a separate `boards/*.h` board-profile directory. Hardware selection is macro-driven in `src/settings.h`.

Current board selection style:

- Define a board/display macro near the top of `settings.h`.
- That macro selects `TFT_CONTROLLER`, `TP_CONTROLLER`, display pins, touch pins, SD_MMC pins, I2S pins, optional IR/Bluetooth pins, and `AMP_ENABLED`.

Target adaptation should add a single new macro block, for example:

- `WAVESHARE_S3_TOUCH_LCD_1_9_PICO_AUDIO`

Do not scatter new GPIO definitions outside this block.

## D. Display And Touch

Existing display support:

- SPI ILI9341 320 x 240.
- SPI ILI9488/ST7796 480 x 320.
- RGB 800 x 480.
- DSI 1024 x 600 and 480 x 800.

Missing for target:

- No ST7789/ST7789V2 controller enum in `lib/tftLib/tft_spi.h`.
- No ST7789V2 init sequence in `lib/tftLib/tft_spi.cpp`.
- No 170 x 320 / 320 x 170 display mode in MiniWebRadio layout.

Existing touch support:

- XPT2046.
- GT911.
- FT6x36.

Missing for target:

- No CST816 driver in `lib/tftLib/`.
- `TP_CONTROLLER` has no CST816 mode.

Waveshare official example evidence:

- LCD SPI pins: RST 9, SCLK 10, DC 11, CS 12, MOSI 13, BL 14.
- LCD size: 170 x 320 normal, 320 x 170 rotated.
- Arduino_GFX ST7789-compatible parameters: width 170, height 320, col offset 35, row offset 0.
- I2C shared bus pins: SDA 47, SCL 48.

Important constraint:

- Do not use the Waveshare example's `FT3168` and `I2C_Touch_ADDR 0x15` for this target. The target touch controller is CST816.

## E. Audio

I2S initialization:

- `src/main.cpp` calls `audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK)`.
- `src/main.cpp` calls `audio.setI2SCommFMT_LSB(I2S_COMM_FMT)`.
- `src/settings.h` default `I2S_COMM_FMT` is `0`, documented as standard format for MAX98357A, PCM5102A, and CS4344.

Amplifier/mute logic:

- `AMP_ENABLED >= 0` is configured as output during setup.
- `HIGH` enables the amplifier.
- `LOW` mutes/disables the amplifier.
- `muteChanged(true)` writes `LOW`.
- `muteChanged(false)` writes `HIGH`.

User-confirmed target audio mapping:

- Pico Audio Pack I2S_DOUT / DATA: GPIO7.
- Pico Audio Pack I2S_BCLK: GPIO15.
- Pico Audio Pack I2S_LRCLK / LRCK: GPIO16.
- AUDIO_MUTE / AMP_ENABLE: `-1`; do not use firmware GPIO mute control.
- I2S_MCLK: `-1`.
- I2S format for first profile: standard I2S, `I2S_COMM_FMT = 0`.

The old flashed firmware confirms MiniWebRadio and ESP32-audioI2S strings. The GPIO numbers are now taken from the user-confirmed old working test and should be used as the only trusted audio mapping for the first Board Profile.

## F. SD Card

Upstream SD_MMC initialization is in `src/main.cpp`:

- For ESP32-S3, it calls `SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0)`.
- It mounts in 1-bit mode with `SD_MMC.begin("/sdcard", true, false, sdmmc_frequency)`.
- For non-S3 with D1 present, it can use 4-bit mode.

Waveshare official SD example:

- SD_CMD = GPIO39.
- SD_D0 = GPIO40.
- SD_CLK = GPIO41.
- 1-bit SDMMC.
- High-speed SDMMC frequency in the example.

For the first target profile, use 1-bit SDMMC and leave D1/D2/D3 as `-1`.

## G. 320 x 170 UI Risk

The risk is high. The project assumes larger display classes and many fixed-coordinate UI controls.

Required changes:

- Add a true 320 x 170 display mode instead of scaling or cropping 320 x 240.
- Add compact layout constants in `src/mwr_src/layout.hpp` or a new included layout file.
- Review header, navigation buttons, station logo, VU meter, file list, settings pages, popups, and keyboard.
- Use a minimal first UI: Now Playing, Radio List, Volume, Wi-Fi Setup, Settings.

First version should prioritize audio stability over complete UI parity.

## H. File-Level Change Plan

Minimal P1/P2 plan:

- `platformio.ini`: add `[env:waveshare-s3-touch-lcd-1_9-pico-audio]`, based on `[env:esp32s3]`, using a 16 MB S3 board/partition.
- `src/settings.h`: add one target macro block for Waveshare 1.9 + Pico Audio, with LCD/SD/I2C known pins and confirmed audio pins.
- `lib/tftLib/tft_spi.h`: add an ST7789/ST7789V2 controller enum.
- `lib/tftLib/tft_spi.cpp`: add ST7789V2 init, 170 x 320 geometry, offsets, rotation handling.
- `lib/tftLib/p_cst816.h` and `lib/tftLib/p_cst816.cpp`: add CST816 touch driver.
- `src/common.h`: add CST816 touch mode selection.
- `src/main.cpp`: add CST816 begin path and preserve SD/I2S init order.
- `src/mwr_src/layout.hpp`: add 320 x 170 layout constants and a minimal page set.
- `docs/pin_mapping.md`: update when audio GPIO is reliably extracted.

Do not start wide UI replacement or LVGL migration in P1/P2.

## I. Required User Information

Still useful before final hardware validation:

- Confirm whether Pico GP22/MUTE is physically isolated from the Waveshare GND header position on the current assembled unit.
- Confirm CST816 reset/interrupt pins if they exist on this board revision; otherwise keep them `-1` and use I2C polling.
- Confirm whether QMI8658 IMU interrupt on IO7 can remain unused, since IO7 is used as I2S_DATA in the audio mapping.

## J. First Stage Steps

1. Add a compile-only target environment with 16 MB flash and 8 MB PSRAM.
2. Add the board macro block with official Waveshare LCD/SD/I2C pins and confirmed Pico Audio Pack pins.
3. Build without changing the UI first.
4. Add ST7789V2 display support and a diagnostic screen.
5. Add CST816 support and a touch diagnostic screen.
6. Mount SD_MMC in 1-bit mode.
7. Enable fixed local audio or fixed stream playback.
8. Only after those pass, start the 320 x 170 MiniWebRadio UI adaptation.
