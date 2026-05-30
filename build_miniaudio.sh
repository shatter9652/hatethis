#!/usr/bin/env bash
set -euo pipefail
rm -rf build
cmake -S . -B build -G Ninja
ninja -C build -j"$(nproc)"
