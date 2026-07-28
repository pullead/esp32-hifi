# MiniWebRadio + LVGL migration

## Ownership

The ESP32-MiniWebRadio core remains responsible for Wi-Fi, decoder tasks,
I2S, SD/MMC, station persistence, Radio Browser, DLNA, WebSocket/Web UI,
NTP, alarms, FTP and reconnect handling.  LVGL is the sole owner of pixels,
touch interpretation, navigation and visual state.

```
LVGL screens (320 x 170)
        | commands / snapshots
PlayerService
        | narrow core adapter
MiniWebRadio core
        | audio task, storage, network, I2S
Pico Audio Pack
```

No LVGL event handler may call `Audio`, `stationManagement`, SD, or Wi-Fi
globals directly.  No legacy `tftLib` page, timer or touch callback may draw
after the LVGL switch is enabled.

## Hardware boundary

| Device | Connection |
| --- | --- |
| LCD | ST7789V2, SPI3_HOST: RST 9, SCLK 10, DC 11, CS 12, MOSI 13, BL 14 |
| Touch | CST816, I2C address `0x15`, SDA 47, SCL 48 |
| SD/MMC | 1-bit: CMD 39, D0 40, CLK 41 |
| Pico Audio Pack | I2S DATA 7, BCLK 15, LRCK 16, no usable MUTE GPIO |

The Pico Audio Pack MUTE pin is physically grounded by the Pico-compatible
header mapping.  The software must not advertise mute GPIO control; audio
mute stays at I2S/software volume level.

## Migration gates

1. PlayerService: command and snapshot layer, including metadata/error events.
2. Board port: direct ST7789 LVGL flush and CST816 LVGL input driver.
3. LVGL shell: Home, Now Playing, Radio, SD and Settings, using a small
   double buffer in PSRAM.
4. Switch: disable all legacy display drawing and touch dispatch in one build.
5. Feature migration: station list, SD browser, volume/EQ, alarms, DLNA and
   Web backend events through PlayerService.
6. Reliability: structured underrun/reconnect logs and a 12-hour soak test.

The old UI is kept only until gate 4. It must not coexist with LVGL at runtime.

## Corrections and decisions (2026-07-23 audit)

- The panel is **ST7789V2 over plain SPI**, not SH8601. Waveshare's own
  ESP-IDF sample for this board reuses the `esp_lcd_sh8601` component name
  and file layout but writes ST7789 init registers over standard SPI, not
  QSPI. Every independently verified path in this repo (`LcdProbe`,
  `IntegratedProbe`, `waveshare_lvgl_port.cpp`) drives `Arduino_ST7789`.
  `src/waveshare/esp_lcd_sh8601.c/.h` is leftover template code, unused by
  the current board port.
- Version decision: the firmware stays on the vendored **LVGL 8.3.11**
  (`lib/lvgl`, gitignored, fetch matching release separately) already wired
  into `src/ui`. LVGL 9.5.0 (`lib/lvgl9/lvgl-9.5.0`, gitignored) is kept only
  for the standalone `probes/lvgl9_probe` display/touch probe and is not a
  pending migration target — do not port `src/ui` to v9 without a new,
  explicit decision to do so.
