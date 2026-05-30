#!/usr/bin/env bash
set -euo pipefail
GAME_DIR="${1:?usage: $0 /path/to/Deltarune}"
find "$GAME_DIR" -type f -path '*/vid/*.mp4' | while read -r mp4; do
  out="${mp4%.*}.ps2.mpg"
  echo "[video] $mp4 -> $out"
  ffmpeg -y -i "$mp4" \
    -vf 'scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2,fps=30' \
    -c:v mpeg1video -b:v 900k -maxrate 1200k -bufsize 1835k \
    -an "$out"
done
