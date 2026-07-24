# Current Hardware Baseline

Date: 2026-07-21

Target hardware:

- Main board: Waveshare ESP32-S3-Touch-LCD-1.9
- MCU: ESP32-S3, QFN56, revision v0.2
- Flash: 16 MB
- PSRAM: 8 MB embedded PSRAM
- Display: ST7789V2, 170 x 320 physical, target 320 x 170 landscape
- Touch: CST816 per Waveshare product documentation
- Audio add-on: Pimoroni Pico Audio Pack, PCM5100A DAC + PAM8908 headphone amp

Board detected on COM5:

- USB VID/PID: 303A:1001
- MAC: 1c:db:d4:7b:51:e8
- Crystal: 40 MHz
- Flash ID: manufacturer 0x20, device 0x4018

Old firmware extraction status:

- Partition table was read successfully from `0x8000`.
- Active boot selection defaults to the `factory` app at `0x10000`.
- `ota_0` at `0x610000` appears blank; its first byte is `0xff`.
- Full 16 MB flash read failed at high baud with `Packet content transfer stopped`.
- Factory app was read successfully in chunks through the first 2 MB.
- The boot log shows the factory image contains these segments:
  - segment 0: `paddr=0x00010020`, size `0x159040`
  - segment 1: `paddr=0x00169068`, size `0x0069c8`
  - segment 2: `paddr=0x0016fa38`, size `0x0005e0`
  - segment 3: `paddr=0x00170020`, size `0x1ef42c`
  - segment 4: `paddr=0x0035f454`, size `0x0180d0`
  - segment 5: `paddr=0x0037752c`, size `0x00001c`

Files saved under `backup/current_firmware/`:

- `partition_table_0x8000.bin`
- `factory_header_0x10000.bin`
- `ota0_header_0x610000.bin`
- `factory_chunk_010000_040000.bin` through `factory_chunk_1d0000_040000.bin`
- `factory_app_first_2mb.bin`

Partition table:

| Name | Type | Subtype | Offset | Size |
| --- | --- | --- | --- | --- |
| nvs | 0x01 | 0x02 | 0x00009000 | 0x00004000 |
| otadata | 0x01 | 0x00 | 0x0000d000 | 0x00002000 |
| phy_init | 0x01 | 0x01 | 0x0000f000 | 0x00001000 |
| factory | 0x00 | 0x00 | 0x00010000 | 0x00600000 |
| ota_0 | 0x00 | 0x10 | 0x00610000 | 0x00600000 |
| ffat | 0x01 | 0x81 | 0x00c10000 | 0x003c0000 |
| coredump | 0x01 | 0x03 | 0x00fd0000 | 0x00030000 |

Old firmware string evidence:

- Project name string: `ESP32-MiniWebRadio`
- Audio library path string: `.pio/libdeps/esp32s3/ESP32-audioI2S/src/Audio.cpp`
- Touch-related string: `CST816 not found at 0x%02X`
- Audio diagnostics strings include sample rate and bits per sample messages.

User-confirmed Pico Audio Pack mapping:

- I2S_DOUT / DATA: ESP32-S3 GPIO7.
- I2S_BCLK / BCLK: ESP32-S3 GPIO15.
- I2S_LRCLK / LRCK: ESP32-S3 GPIO16.
- AUDIO_MUTE / AMP_ENABLE: no ESP32 GPIO, use `-1`.
- Pico GP22 / MUTE maps to GND in the direct header position; the first firmware profile must not attempt GPIO mute control.
- Prior test firmware used `DATA=7`, `BCLK=15`, `LRCK=16`, `MUTE=-1` and produced audio.

Current status:

- No further old-firmware extraction is required for the audio GPIO mapping.
- The next implementation step can use the confirmed audio pins in the target Board Profile.
