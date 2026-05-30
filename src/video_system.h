#pragma once
#include "common.h"
#include "runner.h"
#include "rvalue.h"

void VideoSystem_close(void);
GMLReal VideoSystem_open(Runner* runner, const char* path);
RValue VideoSystem_draw(VMContext* ctx);
void VideoSystem_enableLoop(bool loop);
bool VideoSystem_isLooping(void);
void VideoSystem_setVolume(GMLReal volume);
GMLReal VideoSystem_getVolume(void);
GMLReal VideoSystem_getFormat(void);
GMLReal VideoSystem_getStatus(void);
void VideoSystem_pause(void);
void VideoSystem_resume(void);
void VideoSystem_seekTo(GMLReal ms);
GMLReal VideoSystem_getDuration(void);
GMLReal VideoSystem_getPosition(void);
