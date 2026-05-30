#!/usr/bin/env bash
set -euo pipefail
rm -rf build
cmake -S . -B build -G Ninja -DPLATFORM=glfw -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
