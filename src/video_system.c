#include "video_system.h"
#include "file_system.h"
#include "renderer.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#ifdef USE_FFMPEG_VIDEO
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#endif

typedef enum VideoStatus {
    VIDEO_STATUS_CLOSED = -1,
    VIDEO_STATUS_PLAYING = 0,
    VIDEO_STATUS_FINISHED = 1,
    VIDEO_STATUS_PAUSED = 2,
} VideoStatus;

typedef struct VideoState {
    bool open;
    bool loop;
    bool paused;
    bool finished;
    bool endEventSent;
    GMLReal volume;
    GMLReal durationMs;
    GMLReal positionMs;
    GMLReal fps;
    GMLReal frameTimer;
    bool decodedFirstFrame;
    int width;
    int height;
    int surface;
    Runner* runner;
    char* path;
    GMLReal lastClock;
#ifndef _WIN32
    pid_t audioPid;
#endif
#ifdef USE_FFMPEG_VIDEO
    uint8_t* rgbaFlipBuffer;
#endif
#ifdef USE_FFMPEG_VIDEO
    AVFormatContext* fmt;
    AVCodecContext* codec;
    AVFrame* frame;
    AVFrame* rgbaFrame;
    struct SwsContext* sws;
    uint8_t* rgbaBuffer;
    int videoStream;
    int64_t lastPts;
#endif
} VideoState;

static VideoState gVideo = {0};

static GMLReal nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (GMLReal)ts.tv_sec + ((GMLReal)ts.tv_nsec / (GMLReal)1000000000.0f);
}


#ifdef PLATFORM_PS2
static char* VideoSystem_mapPs2MpegPath(const char* inPath) {
    if (inPath == nullptr) return nullptr;
    size_t n = strlen(inPath);
    char* out = strdup(inPath);
    if (out == nullptr) return nullptr;
    // PS2 cannot decode H.264 MP4. Package videos as PS2-friendly MPEG streams.
    // vid/foo.mp4 -> vid/foo.ps2.mpg
    if (n >= 4 && (strcmp(out + n - 4, ".mp4") == 0 || strcmp(out + n - 4, ".MP4") == 0)) {
        char* mapped = (char*)malloc(n + 6); // replace .mp4 with .ps2.mpg
        if (mapped) {
            memcpy(mapped, out, n - 4);
            strcpy(mapped + n - 4, ".ps2.mpg");
            free(out);
            out = mapped;
        }
    }
    return out;
}
#endif

static RValue makeVideoArray(int status, int surface, int chromaSurface) {
    GMLArray* out = GMLArray_create(3);
    *GMLArray_slot(out, 0) = RValue_makeReal((GMLReal)status);
    *GMLArray_slot(out, 1) = RValue_makeReal((GMLReal)surface);
    *GMLArray_slot(out, 2) = RValue_makeReal((GMLReal)chromaSurface);
    return RValue_makeArray(out);
}

void VideoSystem_close(void) {
#ifndef _WIN32
    if (gVideo.audioPid > 0) {
        kill(gVideo.audioPid, SIGTERM);
        int status = 0;
        waitpid(gVideo.audioPid, &status, WNOHANG);
        gVideo.audioPid = 0;
    }
#endif
#ifdef USE_FFMPEG_VIDEO
    if (gVideo.rgbaFlipBuffer) av_free(gVideo.rgbaFlipBuffer);
    if (gVideo.rgbaBuffer) av_free(gVideo.rgbaBuffer);
    if (gVideo.rgbaFrame) av_frame_free(&gVideo.rgbaFrame);
    if (gVideo.frame) av_frame_free(&gVideo.frame);
    if (gVideo.codec) avcodec_free_context(&gVideo.codec);
    if (gVideo.fmt) avformat_close_input(&gVideo.fmt);
    if (gVideo.sws) sws_freeContext(gVideo.sws);
#endif
    if (gVideo.runner && gVideo.runner->renderer && gVideo.surface >= 0 &&
        gVideo.runner->renderer->vtable->surfaceExists &&
        gVideo.runner->renderer->vtable->surfaceExists(gVideo.runner->renderer, gVideo.surface)) {
        gVideo.runner->renderer->vtable->surfaceFree(gVideo.runner->renderer, gVideo.surface);
    }
    free(gVideo.path);
    memset(&gVideo, 0, sizeof(gVideo));
    gVideo.surface = -1;
    gVideo.volume = (GMLReal)1.0f;
    gVideo.durationMs = (GMLReal)1000.0f;
    gVideo.fps = (GMLReal)30.0f;
}


#ifndef _WIN32
static void VideoSystem_startExternalAudioPlayer(const char* path, GMLReal volume) {
#if defined(USE_FFMPEG_VIDEO) && !defined(PLATFORM_PS2)
    if (path == nullptr || path[0] == '\0') return;
    if (gVideo.audioPid > 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        char volBuf[16];
        int vol = (int)(volume * 100.0);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        snprintf(volBuf, sizeof(volBuf), "%d", vol);
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-fflags", "nobuffer", "-flags", "low_delay", "-sync", "ext", "-volume", volBuf, path, (char*)nullptr);
        _exit(127);
    }
    if (pid > 0) {
        gVideo.audioPid = pid;
        fprintf(stderr, "Video: started external ffplay audio pid=%d\n", (int)pid);
    }
#else
    (void)path; (void)volume;
#endif
}
#endif

#ifdef USE_FFMPEG_VIDEO
static void VideoSystem_flipRGBA(const uint8_t* src, uint8_t* dst, int width, int height, int srcStride) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const int rowBytes = width * 4;
    for (int y = 0; y < height; y++) {
        const uint8_t* srow = src + (height - 1 - y) * srcStride;
        uint8_t* drow = dst + y * rowBytes;
        memcpy(drow, srow, (size_t)rowBytes);
    }
}
#endif

static bool videoAllocSurface(Runner* runner, int w, int h) {
    if (!runner || !runner->renderer || !runner->renderer->vtable->createSurface) return false;
    if (gVideo.surface < 0 || !runner->renderer->vtable->surfaceExists(runner->renderer, gVideo.surface)) {
        gVideo.surface = runner->renderer->vtable->createSurface(runner->renderer, w, h);
    } else if (runner->renderer->vtable->surfaceResize) {
        runner->renderer->vtable->surfaceResize(runner->renderer, gVideo.surface, w, h);
    }
    return gVideo.surface >= 0;
}

GMLReal VideoSystem_open(Runner* runner, const char* relPath) {
    VideoSystem_close();
    gVideo.surface = -1;
    gVideo.volume = (GMLReal)1.0f;
    gVideo.durationMs = (GMLReal)1000.0f;
    gVideo.fps = (GMLReal)30.0f;
    gVideo.runner = runner;
    gVideo.open = true;
    gVideo.paused = false;
    gVideo.finished = false;
    gVideo.positionMs = (GMLReal)0.0f;
    gVideo.frameTimer = (GMLReal)0.0f;
    gVideo.decodedFirstFrame = false;
    gVideo.lastClock = nowSeconds();

    char* resolved = nullptr;
    const char* requestedPath = relPath ? relPath : "";
#ifdef PLATFORM_PS2
    char* ps2MappedPath = VideoSystem_mapPs2MpegPath(requestedPath);
    if (ps2MappedPath) requestedPath = ps2MappedPath;
#endif
    if (runner && runner->fileSystem && runner->fileSystem->vtable->resolvePath) {
        resolved = runner->fileSystem->vtable->resolvePath(runner->fileSystem, requestedPath);
    }
    if (!resolved) resolved = strdup(requestedPath);
#ifdef PLATFORM_PS2
    free(ps2MappedPath);
#endif
    gVideo.path = resolved;

#ifdef USE_FFMPEG_VIDEO
    avformat_network_init();
    if (avformat_open_input(&gVideo.fmt, gVideo.path, NULL, NULL) != 0) {
        fprintf(stderr, "Video: failed to open '%s'\n", gVideo.path);
        gVideo.finished = true;
        return -1.0;
    }
    if (avformat_find_stream_info(gVideo.fmt, NULL) < 0) {
        fprintf(stderr, "Video: failed stream info '%s'\n", gVideo.path);
        gVideo.finished = true;
        return -1.0;
    }
    gVideo.videoStream = -1;
    for (unsigned i = 0; i < gVideo.fmt->nb_streams; i++) {
        if (gVideo.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            gVideo.videoStream = (int)i;
            break;
        }
    }
    if (gVideo.videoStream < 0) {
        fprintf(stderr, "Video: no video stream '%s'\n", gVideo.path);
        gVideo.finished = true;
        return -1.0;
    }
    AVCodecParameters* par = gVideo.fmt->streams[gVideo.videoStream]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        fprintf(stderr, "Video: no decoder for '%s'\n", gVideo.path);
        gVideo.finished = true;
        return -1.0;
    }
    gVideo.codec = avcodec_alloc_context3(dec);
    if (!gVideo.codec || avcodec_parameters_to_context(gVideo.codec, par) < 0 || avcodec_open2(gVideo.codec, dec, NULL) < 0) {
        fprintf(stderr, "Video: decoder open failed '%s'\n", gVideo.path);
        gVideo.finished = true;
        return -1.0;
    }
    gVideo.width = gVideo.codec->width;
    gVideo.height = gVideo.codec->height;
    // Deltarune's video/caption code assumes a stable 30 Hz timeline.
    // Do not run the decoded MP4 at the host draw rate or at odd container rates.
    gVideo.fps = (GMLReal)30.0f;
    if (gVideo.fmt->duration > 0) gVideo.durationMs = (GMLReal)((double)gVideo.fmt->duration * 1000.0 / (double)AV_TIME_BASE);

    gVideo.frame = av_frame_alloc();
    gVideo.rgbaFrame = av_frame_alloc();
    int bufSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, gVideo.width, gVideo.height, 1);
    gVideo.rgbaBuffer = (uint8_t*)av_malloc((size_t)bufSize);
    gVideo.rgbaFlipBuffer = (uint8_t*)av_malloc((size_t)bufSize);
    av_image_fill_arrays(gVideo.rgbaFrame->data, gVideo.rgbaFrame->linesize, gVideo.rgbaBuffer, AV_PIX_FMT_RGBA, gVideo.width, gVideo.height, 1);
    gVideo.sws = sws_getContext(gVideo.width, gVideo.height, gVideo.codec->pix_fmt, gVideo.width, gVideo.height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    videoAllocSurface(runner, gVideo.width, gVideo.height);
#ifndef _WIN32
    VideoSystem_startExternalAudioPlayer(gVideo.path, gVideo.volume);
#endif
    fprintf(stderr, "Video: opened ffmpeg '%s' %dx%d %.2ffps duration=%.0fms surface=%d\n", gVideo.path, gVideo.width, gVideo.height, gVideo.fps, gVideo.durationMs, gVideo.surface);
    return 0.0;
#else
    // Portable fallback for PS2/web/no-FFmpeg builds: never hang forever.
    // It reports a short playable clip and then finishes. A platform backend can replace this later.
    gVideo.width = 640;
    gVideo.height = 480;
    gVideo.durationMs = 1250.0;
    videoAllocSurface(runner, gVideo.width, gVideo.height);
    fprintf(stderr, "Video: opened fallback '%s' surface=%d\n", gVideo.path, gVideo.surface);
    return 0.0;
#endif
}

#ifdef USE_FFMPEG_VIDEO
static bool decodeNextFrame(void) {
    if (!gVideo.fmt || !gVideo.codec || !gVideo.frame) return false;
    AVPacket pkt;
    av_init_packet(&pkt);
    while (av_read_frame(gVideo.fmt, &pkt) >= 0) {
        if (pkt.stream_index != gVideo.videoStream) {
            av_packet_unref(&pkt);
            continue;
        }
        int sr = avcodec_send_packet(gVideo.codec, &pkt);
        av_packet_unref(&pkt);
        if (sr < 0) continue;
        int rr = avcodec_receive_frame(gVideo.codec, gVideo.frame);
        if (rr == 0) {
            sws_scale(gVideo.sws, (const uint8_t* const*)gVideo.frame->data, gVideo.frame->linesize, 0, gVideo.height, gVideo.rgbaFrame->data, gVideo.rgbaFrame->linesize);
            const uint8_t* upload = gVideo.rgbaFrame->data[0];
            if (gVideo.rgbaFlipBuffer != nullptr) {
                VideoSystem_flipRGBA(gVideo.rgbaFrame->data[0], gVideo.rgbaFlipBuffer, gVideo.width, gVideo.height, gVideo.rgbaFrame->linesize[0]);
                upload = gVideo.rgbaFlipBuffer;
            }
            if (gVideo.runner && gVideo.runner->renderer && gVideo.runner->renderer->vtable->updateSurfaceRGBA) {
                gVideo.runner->renderer->vtable->updateSurfaceRGBA(gVideo.runner->renderer, gVideo.surface, upload, gVideo.width, gVideo.height);
            }
            gVideo.decodedFirstFrame = true;
            // Position is wall-clock controlled by VideoSystem_draw().  Do not overwrite it
            // with FFmpeg PTS here; doing so made some 29.97fps videos finish too early
            // or too quickly when the runner's draw loop was faster than 30 Hz.
            return true;
        }
    }
    return false;
}
#endif

static void VideoSystem_sendEndAsyncEvent(VMContext* ctx) {
    if (ctx == nullptr || ctx->runner == nullptr || gVideo.endEventSent) return;
    gVideo.endEventSent = true;

    Runner* runner = ctx->runner;
    DsMapEntry* map = nullptr;
    arrput(runner->dsMapPool, map);
    int32_t mapId = (int32_t)arrlen(runner->dsMapPool) - 1;
    DsMapEntry** mapPtr = &runner->dsMapPool[mapId];

    shput(*mapPtr, safeStrdup("type"), RValue_makeOwnedString(safeStrdup("video_end")));
    shput(*mapPtr, safeStrdup("event_type"), RValue_makeOwnedString(safeStrdup("video_end")));
    shput(*mapPtr, safeStrdup("status"), RValue_makeReal(1.0));

    runner->asyncLoadMapId = mapId;
    // GMS2 async system is normally 75, but some decompilers/exporters label
    // DELTARUNE's couch-video handler as Other_70. Fire both; objects without
    // one of the subevents simply ignore it.
    Runner_executeEventForAll(runner, EVENT_OTHER, 70);
    Runner_executeEventForAll(runner, EVENT_OTHER, OTHER_ASYNC_SYSTEM);

    mapPtr = &runner->dsMapPool[mapId];
    if (*mapPtr != nullptr) {
        repeat(shlen(*mapPtr), j) {
            free((*mapPtr)[j].key);
            RValue_free(&(*mapPtr)[j].value);
        }
        shfree(*mapPtr);
        *mapPtr = nullptr;
    }
    runner->asyncLoadMapId = -1;
}

RValue VideoSystem_draw(VMContext* ctx) {
    (void)ctx;
    if (!gVideo.open) return makeVideoArray(-1, -1, -1);
    if (gVideo.finished) {
        VideoSystem_sendEndAsyncEvent(ctx);
        return makeVideoArray(1, gVideo.surface, -1);
    }

    GMLReal t = nowSeconds();
    if (!gVideo.paused) {
        GMLReal dt = t - gVideo.lastClock;
        if (dt < (GMLReal)0.0f || dt > (GMLReal)1.0f) dt = (GMLReal)1.0f / gVideo.fps;
#ifndef USE_FFMPEG_VIDEO
        gVideo.positionMs += (GMLReal)(dt * (GMLReal)1000.0f);
        if (gVideo.positionMs >= gVideo.durationMs) {
            if (gVideo.loop) gVideo.positionMs = (GMLReal)0.0f;
            else gVideo.finished = true;
        }
#else
        // Advance by wall clock, but only decode/upload frames at the video's FPS.
        // The previous version decoded one frame every draw call, so 29.97fps MP4s
        // played too fast on 60fps displays.
        gVideo.positionMs += (GMLReal)(dt * (GMLReal)1000.0f);
        gVideo.frameTimer += dt;
        const GMLReal frameInterval = (GMLReal)1.0f / (GMLReal)30.0f;
        if (!gVideo.decodedFirstFrame || gVideo.frameTimer >= frameInterval) {
            if (gVideo.frameTimer >= frameInterval) gVideo.frameTimer -= frameInterval;
            if (!decodeNextFrame()) {
                if (gVideo.loop && gVideo.fmt) {
                    av_seek_frame(gVideo.fmt, gVideo.videoStream, 0, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(gVideo.codec);
                    gVideo.positionMs = (GMLReal)0.0f;
                    gVideo.frameTimer = (GMLReal)0.0f;
                    gVideo.decodedFirstFrame = false;
                    decodeNextFrame();
                } else {
                    gVideo.finished = true;
                }
            }
        }
        if (!gVideo.loop && gVideo.durationMs > 0.0 && gVideo.positionMs >= gVideo.durationMs + 250.0) {
            gVideo.finished = true;
        }
#endif
    }
    gVideo.lastClock = t;
    if (gVideo.finished) VideoSystem_sendEndAsyncEvent(ctx);
    return makeVideoArray(gVideo.finished ? 1 : (gVideo.paused ? 3 : 0), gVideo.surface, -1);
}

void VideoSystem_enableLoop(bool loop) { gVideo.loop = loop; }
bool VideoSystem_isLooping(void) { return gVideo.loop; }
void VideoSystem_setVolume(GMLReal volume) {
    gVideo.volume = volume;
    if (gVideo.volume < 0.0) gVideo.volume = 0.0;
    if (gVideo.volume > 1.0) gVideo.volume = (GMLReal)1.0f;
}
GMLReal VideoSystem_getVolume(void) { return gVideo.volume; }
GMLReal VideoSystem_getFormat(void) { return 0.0; }
GMLReal VideoSystem_getStatus(void) { if (!gVideo.open) return -1.0; return gVideo.finished ? 1.0 : (gVideo.paused ? 3.0 : 0.0); }
void VideoSystem_pause(void) { if (gVideo.open) gVideo.paused = true; }
void VideoSystem_resume(void) { if (gVideo.open) { gVideo.paused = false; gVideo.lastClock = nowSeconds(); } }
void VideoSystem_seekTo(GMLReal ms) {
    if (!gVideo.open) return;
    if (ms < 0.0) ms = 0.0;
    if (ms > gVideo.durationMs) ms = gVideo.durationMs;
    gVideo.positionMs = ms;
    gVideo.finished = false;
#ifdef USE_FFMPEG_VIDEO
    if (gVideo.fmt && gVideo.videoStream >= 0) {
        AVRational tb = gVideo.fmt->streams[gVideo.videoStream]->time_base;
        int64_t ts = (int64_t)((ms / 1000.0) / av_q2d(tb));
        av_seek_frame(gVideo.fmt, gVideo.videoStream, ts, AVSEEK_FLAG_BACKWARD);
        if (gVideo.codec) avcodec_flush_buffers(gVideo.codec);
    }
#endif
}
GMLReal VideoSystem_getDuration(void) { return gVideo.durationMs; }
GMLReal VideoSystem_getPosition(void) { return gVideo.positionMs; }
