Butterscotch PS2 video support notes

This source tree adds:
- Fixed modern GL updateSurfaceRGBA() compile error.
- FFmpeg-backed Linux/desktop video path remains available with ENABLE_FFMPEG_VIDEO=ON.
- PS2 video path now maps GameMaker MP4 paths to PS2-friendly MPEG files:
    vid/name.mp4 -> vid/name.ps2.mpg
- PS2 renderer now implements updateSurfaceRGBA() by uploading RGBA8888 frames into a CT16 GS surface.
- PS2 CMake option:
    -DENABLE_PS2_LIBMPEG_VIDEO=ON
  This links against ps2sdk's libmpeg library name `mpeg` and enables USE_PS2_LIBMPEG_VIDEO.

Recommended PS2 encode:
ffmpeg -i vid/tennaIntroF1_compressed_28.mp4 \
  -vf scale=320:240,fps=24 \
  -c:v mpeg1video -b:v 900k -an \
  vid/tennaIntroF1_compressed_28.ps2.mpg

Notes:
- H.264 MP4 is not realistic on PS2; convert videos during packaging.
- This keeps video_open/video_draw API working across platforms.
