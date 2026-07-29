#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FONT_DIR="$PROJECT_ROOT/src/ui/fonts"
FONT_PATH="${1:-$PROJECT_ROOT/tools/fonts/NotoSansSC-VF.ttf}"
LV_FONT_CONV="${LV_FONT_CONV:-lv_font_conv}"
BPP="${BPP:-4}"

if [[ ! -f "$FONT_PATH" ]]; then
  cat >&2 <<EOF
Font file not found: $FONT_PATH

Pass the font path as the first argument, or place NotoSansSC-VF.ttf at:
  $PROJECT_ROOT/tools/fonts/NotoSansSC-VF.ttf
EOF
  exit 1
fi

if ! command -v "$LV_FONT_CONV" >/dev/null 2>&1; then
  echo "lv_font_conv not found. Install it first, for example: npm install -g lv_font_conv@1.5.3" >&2
  exit 1
fi

"$LV_FONT_CONV" \
  --font "$FONT_PATH" \
  --size 13 \
  --bpp "$BPP" \
  --format lvgl \
  --lv-include lvgl.h \
  --no-compress \
  -o "$FONT_DIR/lv_font_cjk_13.c" \
  --range 0x20-0x7F \
  --symbols "$(cat "$FONT_DIR/cjk_symbols.txt")"

"$LV_FONT_CONV" \
  --font "$FONT_PATH" \
  --size 16 \
  --bpp "$BPP" \
  --format lvgl \
  --lv-include lvgl.h \
  --no-compress \
  -o "$FONT_DIR/lv_font_cjk_16.c" \
  --range 0x20-0x7F \
  --symbols "$(cat "$FONT_DIR/cjk_symbols_16.txt")"

echo "Fonts regenerated from $FONT_PATH with bpp=$BPP"
