# Phase 1 Migration Status

Date: 2026-07-22

## Target

- Board: Waveshare ESP32-S3-Touch-LCD-1.9
- Display: ST7789V2 TFT LCD
- Touch: CST816
- Landscape resolution: 320 x 170
- Audio board: Pico Audio Pack / PCM5100A path

## Confirmed Audio Mapping

- I2S DATA / DOUT: GPIO7
- I2S BCLK: GPIO15
- I2S LRCLK: GPIO16
- MCLK: not used
- AMP / MUTE GPIO: not controlled by firmware (`-1`)

## Implemented

- Added PlatformIO environment `waveshare-s3-touch-lcd-1_9-pico-audio`.
- Added board profile guarded by `WAVESHARE_S3_TOUCH_LCD_1_9_PICO_AUDIO`.
- Added SPI ST7789V2 support path for 170 x 320 panel with 320 x 170 landscape layout.
- Added CST816 touch driver path.
- Added compact 320 x 170 layout constants for MiniWebRadio UI.
- Fixed build configuration for Arduino autostart, FreeRTOS tick rate, and mbedTLS PSK modes.
- Fixed current framework link issue by using `ESP.getPsramSize()` for PSRAM logging.

## Build Result

PlatformIO build succeeded for:

```text
waveshare-s3-touch-lcd-1_9-pico-audio
```

Generated artifacts:

```text
C:\b3\waveshare-s3-touch-lcd-1_9-pico-audio\bootloader.bin
C:\b3\waveshare-s3-touch-lcd-1_9-pico-audio\partitions.bin
C:\b3\waveshare-s3-touch-lcd-1_9-pico-audio\firmware.bin
C:\b3\waveshare-s3-touch-lcd-1_9-pico-audio\firmware.factory.bin
```

## Known Warnings / Next Checks

- PlatformIO reported: expected 16MB flash, detected 2MB flash. Verify the board flash configuration or esptool probe before flashing.
- Local PlatformIO core is currently stored at `C:\pioh` to avoid Windows command-line length issues.
- The local ESP-IDF package under `C:\pioh` contains a build-only workaround that excludes the unused RGB LCD panel source because GCC 14.2.0 hit an internal compiler error on this target.
- Next phase should be physical flashing and serial log validation: LCD init, touch coordinates, SD mount, I2S output, and Wi-Fi/radio playback.
