Butterscotch game_change build modes

Default / experimental in-process mode:
  ./build_inprocess_mode.sh
or:
  cmake -S . -B build -G Ninja -DPLATFORM=glfw -DAUDIO_BACKEND=miniaudio -DBUTTERSCOTCH_GAME_CHANGE_MODE=inprocess

DELTARUNNER-like spawn/restart mode:
  ./build_deltarunner_mode.sh
or:
  cmake -S . -B build -G Ninja -DPLATFORM=glfw -DAUDIO_BACKEND=miniaudio -DBUTTERSCOTCH_GAME_CHANGE_MODE=deltarunner

Mode behavior:
- inprocess: game_change hot-swaps chapter data.win inside the same process/window.
- deltarunner: game_change launches the same runner with the target chapter data.win and -gamedir, then exits the old process.

For PS2 experiments, build with:
  cmake -S . -B build -DPLATFORM=ps2 -DAUDIO_BACKEND=ps2 -DBUTTERSCOTCH_GAME_CHANGE_MODE=deltarunner

The deltarunner mode is intentionally closer to DELTARUNNER's Linux game_change, while inprocess is the replacement path to keep fixing.
