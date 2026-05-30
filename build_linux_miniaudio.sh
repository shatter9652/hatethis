#!/usr/bin/env bash
set -euo pipefail
rm -rf build
cmake -S . -B build -G Ninja \
  -DPLATFORM=glfw \
  -DAUDIO_BACKEND=miniaudio \
  -DBUTTERSCOTCH_GAME_CHANGE_MODE=inprocess \
  -DCMAKE_BUILD_TYPE=Debug
ninja -C build -j"$(nproc)"
