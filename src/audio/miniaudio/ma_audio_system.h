#pragma once

#include "common.h"
#include "audio_system.h"
#include "miniaudio.h"

#define MAX_SOUND_INSTANCES 128
#define SOUND_INSTANCE_ID_BASE 100000
#define MAX_AUDIO_STREAMS 32
// This is the index space that the native runner uses
#define AUDIO_STREAM_INDEX_BASE 300000

typedef struct {
    bool active;
    int32_t soundIndex; // SOND resource that spawned this
    int32_t instanceId; // unique ID returned to GML
    ma_sound maSound; // miniaudio sound object
    ma_decoder decoder; // decoder for memory-based audio
    bool ownsDecoder; // true if decoder needs uninit
    ma_audio_buffer audioBuffer; // fully-decoded external stream buffer
    void* pcmData; // owned PCM data for audioBuffer
    bool ownsAudioBuffer; // true if audioBuffer/pcmData need cleanup
    float targetGain;
    float currentGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float startGain;
    int32_t priority;
} SoundInstance;

typedef struct {
    bool active;
    char* filePath; // resolved file path (owned, freed on destroy)
} AudioStreamEntry;

typedef struct {
    AudioSystem base;
    ma_engine engine;
    bool engineInitialized;
    SoundInstance instances[MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;
    FileSystem* fileSystem;
    AudioStreamEntry streams[MAX_AUDIO_STREAMS];
    // After in-process game_change, some DELTARUNE chapter scripts create the
    // music stream but fail to reach the play call under this runner. Let the
    // first music streams auto-start, and de-dupe later audio_play_sound().
    bool autoStartMusicStreamsAfterGameChange;
    bool afterGameChange;
} MaAudioSystem;

MaAudioSystem* MaAudioSystem_create(void);
