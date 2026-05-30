Audio fix test build

The miniaudio backend is creating/starting sound handles, but chapter audio is still silent.
This package keeps the in-process game_change implementation, but adds an OpenAL build path so Linux testing uses OpenAL Soft instead of miniaudio.

Build the recommended test version:
  ./build_linux_openal.sh

Run:
  ./build/butterscotch ~/UTDR/Deltarune/chapter1_windows/data.win
  ./build/butterscotch ~/UTDR/Deltarune/data.win

Fallback miniaudio build:
  ./build_linux_miniaudio.sh

Arch deps if OpenAL is missing:
  sudo pacman -S openal
