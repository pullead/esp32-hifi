# LVGL UI boundary

`PlayerService` is the boundary between the future LVGL screens and the
MiniWebRadio core.  It accepts user commands and exposes a copy-only
`PlayerSnapshot`; LVGL must not read or write decoder, Wi-Fi, SD or station
globals directly.

The staged migration is intentional:

1. `player_service.*` gives LVGL a stable playback contract.
2. `waveshare_lvgl_port.*` owns SH8601 drawing and CST816 input.
3. LVGL becomes the only renderer and input consumer.
4. Legacy `tftLib` placement and touch dispatch are compiled out, while the
   MiniWebRadio audio/network tasks continue unchanged.

This separation keeps the upstream audio and server code mergeable and makes
the LVGL application replaceable without touching decoder timing.
