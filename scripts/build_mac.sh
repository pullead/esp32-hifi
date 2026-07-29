#!/usr/bin/env bash
set -euo pipefail

ENVIRONMENT="${1:-esp32s3_OTA}"
JOBS="${JOBS:-1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="$ROOT"

if [[ "$ROOT" =~ [[:space:]] ]]; then
  LINK="${MWR_BUILD_LINK:-/tmp/mwr-src}"
  if [[ -e "$LINK" && ! -L "$LINK" ]]; then
    echo "$LINK exists and is not a symlink; set MWR_BUILD_LINK to another path." >&2
    exit 1
  fi
  ln -sfn "$ROOT" "$LINK"
  WORKDIR="$LINK"
fi

PIO="${PLATFORMIO:-pio}"
if ! command -v "$PIO" >/dev/null 2>&1; then
  if command -v platformio >/dev/null 2>&1; then
    PIO="platformio"
  else
    echo "PlatformIO was not found. Install it or add pio to PATH." >&2
    exit 1
  fi
fi

cd "$WORKDIR"
"$PIO" run -e "$ENVIRONMENT" -j "$JOBS"
