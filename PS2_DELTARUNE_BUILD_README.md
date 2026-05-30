# DELTARUNE PS2 build/ISO notes

This tree keeps in-process `game_change`, so the launcher `data.win` can switch into
`chapter1_windows/data.win`, `chapter2_windows/data.win`, etc. inside one ELF.

## Build ELF

```bash
export PS2DEV=/path/to/ps2dev
export PS2SDK=$PS2DEV/ps2sdk
./tools/ps2/build_ps2_elf.sh
```

The CMake PS2 path enables:

```text
-DPLATFORM=ps2
-DAUDIO_BACKEND=ps2
-DENABLE_PS2_LIBMPEG_VIDEO=ON
-DBUTTERSCOTCH_GAME_CHANGE_MODE=inprocess
```

## Preprocess all chapters

Use the attached ButterscotchPreprocessor tree:

```bash
./tools/ps2/preprocess_deltarune_ps2.sh ~/UTDR/Deltarune /path/to/ButterscotchPreprocessor-main ps2iso_root
cp build-ps2/butterscotch ps2iso_root/BUTTER.ELF
./tools/ps2/make_ps2_iso.sh ps2iso_root deltarune_ps2.iso
```

## Video layout

Desktop still opens the original MP4 files.

PS2 maps:

```text
vid/foo.mp4 -> vid/foo.ps2.mpg
```

The conversion script generates MPEG-1 video at 320x240/30fps with no embedded audio.
Video audio should be handled as separate game/audio assets later; H.264 MP4 is not
realistic on PS2.
