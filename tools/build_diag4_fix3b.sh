#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/diagnostics/0.4.2-diag4-fix3b"
BASE_REV="5fcac9fcee67b22ebab1f6064255cfa5f7558ae2"
if [[ -n "${PIO_BIN:-}" ]]; then
  PIO="$PIO_BIN"
elif [[ -x "$HOME/.local/bin/pio" ]]; then
  PIO="$HOME/.local/bin/pio"
else
  PIO="pio"
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/src"
git -C "$ROOT" archive "$BASE_REV" | tar -x -C "$TMP/src"
git -C "$TMP/src" apply "$OUT/hardware-diagnostics.patch"
(
  cd "$TMP/src"
  "$PIO" run -e esp32s3_n16r8_diag
  python3 tools/check_firmware_manifest.py \
    .pio/build/esp32s3_n16r8_diag/firmware.bin \
    --expected-version 0.4.2-fix3b
  cp .pio/build/esp32s3_n16r8_diag/firmware.bin \
    "$OUT/h2-gauge-v0.4.2-fix3b-esp32s3-n16r8.bin"
)
sha256sum "$OUT/h2-gauge-v0.4.2-fix3b-esp32s3-n16r8.bin" \
  | tee "$OUT/SHA256SUMS.txt"
