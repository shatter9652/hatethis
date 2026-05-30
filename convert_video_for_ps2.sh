#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -lt 1 ]; then
  echo "usage: $0 input.mp4 [output.ps2.mpg]" >&2
  exit 2
fi
in="$1"
out="${2:-${in%.*}.ps2.mpg}"
ffmpeg -y -i "$in" \
  -vf 'scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,fps=24' \
  -c:v mpeg1video -b:v 900k -maxrate 1200k -bufsize 1835k \
  -an "$out"
echo "wrote $out"
