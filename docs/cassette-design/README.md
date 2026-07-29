# Cassette view bitmap assets

`cassette_body.html` (320x170) and `reel.html` (60x60) are HTML/CSS mockups
rendered with headless Chrome, then converted to raw RGB565 C arrays for
`src/ui/images/`. Original design, not a copy of any specific reference
image -- cream body, dark reel windows, orange tape-position needle.

The reel is a separate small image (not baked into the body) so it can
actually rotate via `lv_img_set_angle()` at runtime, clipped to a circle by
an LVGL wrapper object -- see `buildCassetteVisual()` in `hifi_ui.cpp`.

## Regenerate

```bash
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --headless --disable-gpu --window-size=320,170 \
  --screenshot=cassette_body_shot.png cassette_body.html

"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --headless --disable-gpu --window-size=60,60 \
  --screenshot=reel_shot2.png reel.html

python3 png_to_rgb565.py cassette_body_shot.png ../../src/ui/images/cassette_body_data.c cassette_body
python3 png_to_rgb565.py reel_shot2.png ../../src/ui/images/cassette_reel_data.c cassette_reel
```

`png_to_rgb565.py` needs Pillow (`pip3 install Pillow`). Output matches
`LV_COLOR_DEPTH 16` / `LV_COLOR_16_SWAP 0` (see `src/lv_conf.h`).
