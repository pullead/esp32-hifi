# Pin Mapping

This file records only values with a clear source. Do not fill unknown audio pins from Raspberry Pi Pico defaults.

## Waveshare ESP32-S3-Touch-LCD-1.9

Source: Waveshare `ESP32-S3-LCD-1.9` official example repository, Arduino LVGL and SD examples.

| Signal | GPIO | Source | Status |
| --- | ---: | --- | --- |
| LCD_RST | 9 | `02_Example/Arduino/08_LVGL_Test/lcd_config.h` | Confirmed from official example |
| LCD_SCLK | 10 | `02_Example/Arduino/08_LVGL_Test/lcd_config.h` | Confirmed from official example |
| LCD_DC | 11 | `02_Example/Arduino/08_LVGL_Test/lcd_config.h` | Confirmed from official example |
| LCD_CS | 12 | `02_Example/Arduino/08_LVGL_Test/lcd_config.h` | Confirmed from official example |
| LCD_MOSI | 13 | `02_Example/Arduino/08_LVGL_Test/lcd_config.h` | Confirmed from official example |
| LCD_BL | 14 | `02_Example/Arduino/08_LVGL_Test/lcd_config.h` | Confirmed from official example |
| TOUCH_SDA | 47 | `02_Example/Arduino/02_I2C_QMI8658/i2c_bsp.cpp` | Shared I2C bus confirmed |
| TOUCH_SCL | 48 | `02_Example/Arduino/02_I2C_QMI8658/i2c_bsp.cpp` | Shared I2C bus confirmed |
| TOUCH_RST | -1 | Not found in trusted CST816 source yet | Unknown |
| TOUCH_INT | -1 | Not found in trusted CST816 source yet | Unknown |
| SD_CMD | 39 | `02_Example/Arduino/03_SD_Card/sd_card_bsp.cpp` | Confirmed from official example |
| SD_D0 | 40 | `02_Example/Arduino/03_SD_Card/sd_card_bsp.cpp` | Confirmed from official example |
| SD_CLK | 41 | `02_Example/Arduino/03_SD_Card/sd_card_bsp.cpp` | Confirmed from official example |
| SD_D1 | -1 | Official example uses 1-bit SDMMC | Not connected/unused for first profile |
| SD_D2 | -1 | Official example uses 1-bit SDMMC | Not connected/unused for first profile |
| SD_D3 | -1 | Official example uses 1-bit SDMMC | Not connected/unused for first profile |

Notes:

- The Waveshare Arduino LVGL example currently contains `FT3168` and `I2C_Touch_ADDR 0x15`. Do not use that touch-controller identity or address for this target profile.
- Target touch controller remains CST816, per product documentation and old firmware string evidence.
- LCD panel parameters from official examples: ST7789-compatible 170 x 320 IPS with column offset 35, row offset 0.

## Pico Audio Pack

Source priority: old working firmware only. These audio pins are now accepted as user-provided, previously tested hardware facts from the old working firmware and Pico Audio Pack wiring test.

| Signal | GPIO | Source | Status |
| --- | ---: | --- | --- |
| I2S_DOUT | 7 | User-confirmed old working firmware/test: Pico GP9 / I2S_DATA to ESP32-S3 IO7 | Confirmed by playback test |
| I2S_BCLK | 15 | User-confirmed old working firmware/test: Pico GP10 / I2S_BCLK to ESP32-S3 IO15 | Confirmed by playback test |
| I2S_LRCLK | 16 | User-confirmed old working firmware/test: Pico GP11 / I2S_LRCK to ESP32-S3 IO16 | Confirmed by playback test |
| AUDIO_MUTE / AMP_ENABLE | -1 | Pico GP22 / MUTE maps to GND on this plugged-in header position; firmware must not drive it | Confirmed by prior design/test |

Physical Pico Audio Pack mapping used by the old working firmware:

- Pico GP9 / I2S_DATA / physical pin 12 -> ESP32-S3 IO7.
- Pico GP10 / I2S_BCLK / physical pin 14 -> ESP32-S3 IO15.
- Pico GP11 / I2S_LRCK / physical pin 15 -> ESP32-S3 IO16.
- Pico GP22 / MUTE / physical pin 29 -> GND on the Waveshare header position.

Important MUTE note:

- Do not assign an ESP32 GPIO to `AUDIO_MUTE` for this first profile.
- Use `AMP_ENABLED = -1`.
- The tested working configuration was `DATA=7`, `BCLK=15`, `LRCK=16`, `MUTE=-1`.
- If GP22/MUTE is physically isolated from the Waveshare header, the Pico Audio Pack pull-up can enable the DAC/headphone amp. If it is not isolated and remains tied to GND, the hardware MUTE path may force mute.

Audio behavior confirmed from ESP32-MiniWebRadio source:

- `audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK)` is called during setup.
- `audio.setI2SCommFMT_LSB(I2S_COMM_FMT)` selects standard I2S when `I2S_COMM_FMT == 0`.
- `AMP_ENABLED` is driven `HIGH` to enable and `LOW` to mute/off in the upstream control logic.

Audio defaults for first Board Profile:

- I2S communication format: standard I2S, `I2S_COMM_FMT = 0`.
- I2S_MCLK: `-1`.
- Slot bit width: keep ESP32-audioI2S default for PCM510x-compatible output unless verified otherwise.
- Active data bit width: source/decoder driven through ESP32-audioI2S.
- Channel format: stereo.
- Sample rates: source-driven; must verify at least 44.1 kHz and 48 kHz during P4 audio tests.
