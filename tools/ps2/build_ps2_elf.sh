#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
: "${PS2DEV:?set PS2DEV first}"
: "${PS2SDK:?set PS2SDK first}"
export PATH="$PS2DEV/bin:$PS2DEV/ee/bin:$PS2SDK/bin:$PATH"
mkdir -p "$ROOT/build-ps2"
cmake -S "$ROOT" -B "$ROOT/build-ps2" \
  -DPLATFORM=ps2 \
  -DAUDIO_BACKEND=ps2 \
  -DENABLE_PS2_LIBMPEG_VIDEO=ON \
  -DBUTTERSCOTCH_GAME_CHANGE_MODE=inprocess \
  -DENABLE_FFMPEG_VIDEO=OFF \
  -DENABLE_WAD17=ON -DENABLE_WAD16=ON -DENABLE_WAD14=OFF \
  "$@"
cmake --build "$ROOT/build-ps2" -j"$(nproc)"
echo "ELF should be at: $ROOT/build-ps2/butterscotch"
