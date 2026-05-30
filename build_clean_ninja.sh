#!/usr/bin/env bash
set -euo pipefail
MODE="${BUTTERSCOTCH_GAME_CHANGE_MODE:-inprocess}"
PLATFORM_VALUE="${PLATFORM_BACKEND:-glfw}"
AUDIO_VALUE="${AUDIO_BACKEND:-miniaudio}"
rm -rf build
cmake -S . -B build -G Ninja \
  -DPLATFORM="$PLATFORM_VALUE" \
  -DAUDIO_BACKEND="$AUDIO_VALUE" \
  -DBUTTERSCOTCH_GAME_CHANGE_MODE="$MODE"
ninja -C build -j"$(nproc)"
