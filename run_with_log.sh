#!/usr/bin/env bash
set -euo pipefail
BIN="${BIN:-./build/butterscotch}"
rm -f butterscotch_terminal.log butterscotch_debug.log
"$BIN" "$@" 2>&1 | tee -a butterscotch_terminal.log
