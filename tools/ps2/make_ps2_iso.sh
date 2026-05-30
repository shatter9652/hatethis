#!/usr/bin/env bash
set -euo pipefail
ISO_ROOT="${1:?usage: $0 ps2iso_root output.iso}"
OUT_ISO="${2:?usage: $0 ps2iso_root output.iso}"
if command -v mkisofs >/dev/null 2>&1; then
  MKISOFS=mkisofs
elif command -v genisoimage >/dev/null 2>&1; then
  MKISOFS=genisoimage
else
  echo "Install mkisofs or genisoimage" >&2
  exit 1
fi
"$MKISOFS" -o "$OUT_ISO" -V DELTARUNE -sysid PLAYSTATION -l -allow-lowercase "$ISO_ROOT"
echo "Wrote $OUT_ISO"
