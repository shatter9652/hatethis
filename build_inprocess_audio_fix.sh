#!/usr/bin/env bash
set -euo pipefail
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build -j"$(nproc)"
