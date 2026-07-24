# Phase 1 Migration Status

Date: 2026-07-21

Target environment:
- PlatformIO env: `waveshare-s3-touch-lcd-1_9-pico-audio`
- Board: Waveshare ESP32-S3-Touch-LCD-1.9
- Display: ST7789V2, physical 170x320, target landscape 320x170
- Touch: CST816
- Audio: Pimoroni Pico Audio Pack with confirmed wiring

Confirmed audio pins used by firmware:
- I2S_DOUT: GPIO7
- I2S_BCLK: GPIO15
- I2S_LRCLK: GPIO16
- I2S_MCLK: -1
- AMP_ENABLED / MUTE: -1
- I2S format: `I2S_COMM_FMT = 0`

Implemented in source:
- Added dedicated PlatformIO target environment.
- Added a dedicated board macro `WAVESHARE_S3_TOUCH_LCD_1_9_PICO_AUDIO`.
- Added ST7789V2 controller ID `TFT_CONTROLLER = 11`.
- Added CST816 touch mode `TP_CONTROLLER = 9`.
- Added compact 320x170 layout profile.
- Added 1-bit SDMMC pin mapping: CMD GPIO39, D0 GPIO40, CLK GPIO41.
- Added SPI LCD pin mapping: RST GPIO9, SCLK GPIO10, DC GPIO11, CS GPIO12, MOSI GPIO13, BL GPIO14.
- Added I2C touch pins: SDA GPIO47, SCL GPIO48.

Verification status:
- PlatformIO Core was installed using the bundled Codex Python.
- The global `C:\Users\tei_s\.platformio` directory reported a permissions/owner error, so verification was redirected to project-local `.pio-core`.
- Required ESP32 platform packages and `ESP32-audioI2S` were downloaded.
- Full compile did not reach source compilation before timeout; logs stopped after dependency/tool installation.
- No firmware image has been produced yet.

Next verification step:
Run the build again after PlatformIO finishes or reuses the local `.pio-core` environment:

```powershell
$env:PLATFORMIO_CORE_DIR=(Resolve-Path .).Path + '\.pio-core'
$env:PLATFORMIO_DISABLE_TELEMETRY='1'
& 'C:\Users\tei_s\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' -m platformio run -e waveshare-s3-touch-lcd-1_9-pico-audio
```
