#!/usr/bin/env bash
set -euo pipefail
GAME_DIR="${1:?usage: $0 /path/to/Deltarune /path/to/ButterscotchPreprocessor-main [outdir]}"
PREPROC_DIR="${2:?usage: $0 /path/to/Deltarune /path/to/ButterscotchPreprocessor-main [outdir]}"
OUT="${3:-ps2iso_root}"
ROOT="$(pwd)"
mkdir -p "$OUT"
cd "$PREPROC_DIR"
./gradlew :processor-cli:installDist
CLI="$PREPROC_DIR/processor-cli/build/install/processor-cli/bin/butterscotch-preprocessor"
cd "$ROOT"
process_one() {
  local name="$1" dir="$2"
  mkdir -p "$OUT/$dir"
  "$CLI" "$GAME_DIR/$dir/data.win" --target ps2 -o "$OUT/$dir/PS2DATA"
  cp "$GAME_DIR/$dir/data.win" "$OUT/$dir/DATA.WIN"
  find "$GAME_DIR/$dir" -maxdepth 1 -type f \( -name '*.ogg' -o -name 'audiogroup*.dat' -o -name 'options.ini' \) -exec cp -v '{}' "$OUT/$dir/" ';'
  [ -d "$GAME_DIR/$dir/lang" ] && cp -a "$GAME_DIR/$dir/lang" "$OUT/$dir/"
  [ -d "$GAME_DIR/$dir/vid" ] && cp -a "$GAME_DIR/$dir/vid" "$OUT/$dir/"
}
# launcher/chapter select
mkdir -p "$OUT"
"$CLI" "$GAME_DIR/data.win" --target ps2 -o "$OUT/PS2DATA"
cp "$GAME_DIR/data.win" "$OUT/DATA.WIN"
[ -d "$GAME_DIR/mus" ] && cp -a "$GAME_DIR/mus" "$OUT/"
for ch in chapter1_windows chapter2_windows chapter3_windows chapter4_windows; do
  [ -f "$GAME_DIR/$ch/data.win" ] && process_one "$ch" "$ch"
done
"$(dirname "$0")/convert_deltarune_videos_ps2.sh" "$OUT"
cat > "$OUT/SYSTEM.CNF" <<'CNF'
BOOT2 = cdrom0:\BUTTER.ELF;1
VER = 1.00
VMODE = NTSC
CNF
echo "Preprocessed PS2 ISO root written to $OUT"
echo "Copy your built ELF to $OUT/BUTTER.ELF, then run make_ps2_iso.sh"
