// On Windows, include windows.h first so its headers are processed before stb_vorbis
// defines single-letter macros (L, C, R) that conflict with winnt.h struct field names.
#ifdef _WIN32
#include <windows.h>
#include "debug_log.h"
#endif

// Include stb_vorbis BEFORE miniaudio so that STB_VORBIS_INCLUDE_STB_VORBIS_H is defined,
// which enables miniaudio's built-in OGG Vorbis decoding support.
#include "stb_vorbis.c"

// Butterscotch/DELTARUNE hot-swaps data.win in-process. miniaudio's
// debug assertions can abort on harmless internal resampler edge cases
// during/after chapter switching. Keep runtime errors return-code based.
#ifndef MA_ASSERT
#define MA_ASSERT(condition) ((void)0)
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ma_audio_system.h"
#include "data_win.h"
#include "utils.h"
#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "stb_ds.h"

// ===[ Helpers ]===

static void cleanupSoundInstance(SoundInstance* inst) {
    if (inst == nullptr) return;
    ma_sound_uninit(&inst->maSound);
    if (inst->ownsDecoder) {
        ma_decoder_uninit(&inst->decoder);
        inst->ownsDecoder = false;
    }
    if (inst->ownsAudioBuffer) {
        ma_audio_buffer_uninit(&inst->audioBuffer);
        free(inst->pcmData);
        inst->pcmData = nullptr;
        inst->ownsAudioBuffer = false;
    }
    inst->active = false;
}

static ma_result initFileSound(MaAudioSystem* ma, SoundInstance* slot, const char* path, bool streamFromDisk) {
    memset(slot, 0, sizeof(*slot));

    if (ma == nullptr || path == nullptr || path[0] == '\0') {
        return MA_INVALID_ARGS;
    }
    if (!ma->engineInitialized) {
        BSC_debugLog("audio-error", "initFileSound while engine is not initialized path=%s", path);
        return MA_INVALID_OPERATION;
    }

    // Do NOT use ma_sound_init_from_file() here.
    // DELTARUNE chapters create/destroy/recreate the same external OGGs a lot
    // (especially Ch3/Ch4 transitions). miniaudio's resource-manager file cache
    // was hitting a use-after-free in ma_resource_manager_data_buffer_node_acquire.
    // A per-instance decoder avoids that global cache entirely and matches the
    // embedded-AUDO path: the decoder lives exactly as long as this SoundInstance.
    ma_decoder_config decoderConfig = ma_decoder_config_init_default();
    ma_result result = ma_decoder_init_file(path, &decoderConfig, &slot->decoder);
    if (result != MA_SUCCESS) {
        BSC_debugLog("audio-error", "ma_decoder_init_file failed result=%d stream=%d path=%s", (int)result, streamFromDisk ? 1 : 0, path);
        return result;
    }
    slot->ownsDecoder = true;

    ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
    result = ma_sound_init_from_data_source(&ma->engine, &slot->decoder, flags, nullptr, &slot->maSound);
    if (result != MA_SUCCESS) {
        BSC_debugLog("audio-error", "ma_sound_init_from_data_source(file decoder) failed result=%d stream=%d path=%s", (int)result, streamFromDisk ? 1 : 0, path);
        ma_decoder_uninit(&slot->decoder);
        slot->ownsDecoder = false;
        return result;
    }

    slot->ownsAudioBuffer = false;
    slot->pcmData = nullptr;
    BSC_debugLog("audio-init-sound", "file decoder sound ready stream=%d path=%s", streamFromDisk ? 1 : 0, path);
    return MA_SUCCESS;
}

static SoundInstance* findFreeSlot(MaAudioSystem* ma) {
    // First pass: find an inactive slot
    repeat(MAX_SOUND_INSTANCES, i) {
        if (!ma->instances[i].active) {
            return &ma->instances[i];
        }
    }

    // Second pass: evict the lowest-priority ended sound
    SoundInstance* best = nullptr;
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!ma_sound_is_playing(&inst->maSound)) {
            if (best == nullptr || best->priority > inst->priority) {
                best = inst;
            }
        }
    }

    if (best != nullptr) {
        cleanupSoundInstance(best);
    }

    return best;
}

static SoundInstance* findInstanceById(MaAudioSystem* ma, int32_t instanceId) {
    int32_t slotIndex = instanceId - SOUND_INSTANCE_ID_BASE;
    if (0 > slotIndex || slotIndex >= MAX_SOUND_INSTANCES) return nullptr;
    SoundInstance* inst = &ma->instances[slotIndex];
    if (!inst->active || inst->instanceId != instanceId) return nullptr;
    return inst;
}
static SoundInstance* findActiveInstanceForSound(MaAudioSystem* ma, int32_t soundIndex) {
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == soundIndex) {
            return inst;
        }
    }
    return nullptr;
}

static bool isLikelyMusicStreamPath(const char* path) {
    if (path == nullptr) return false;
    return strstr(path, "/mus/") != nullptr || strstr(path, "\\mus\\") != nullptr ||
           strncmp(path, "mus/", 4) == 0 || strncmp(path, "mus\\", 4) == 0;
}


// Helper: resolve external audio file path from Sound entry
static char* resolveExternalPath(MaAudioSystem* ma, Sound* sound) {
    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    // If the filename has no extension, append ".ogg"
    bool hasExtension = (strchr(file, '.') != nullptr);

    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file);
    }

    return ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
}

// ===[ Vtable Implementations ]===

static void maInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Runner_create() is called both at real process start and after in-process
    // game_change. For in-process chapter switches, DO NOT destroy/recreate the
    // miniaudio device here. On PulseAudio/ALSA that can leave the replacement
    // engine apparently valid but silent. Keep the device/engine alive and only
    // swap the GameMaker sound tables.
    arrput(ma->base.audioGroups, dataWin);
    ma->fileSystem = fileSystem;

    if (!ma->engineInitialized) {
        ma_engine_config config = ma_engine_config_init();
        config.channels = 2;
        config.sampleRate = 48000;
        config.periodSizeInFrames = 1024;

        ma_result result = ma_engine_init(&config, &ma->engine);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to initialize miniaudio engine (error %d)\n", result);
            BSC_debugLog("audio-error", "ma_engine_init failed result=%d", (int)result);
            return;
        }
        ma->engineInitialized = true;
    }

    ma_engine_set_volume(&ma->engine, 1.0f);
    ma_result startResult = ma_engine_start(&ma->engine);
    if (startResult != MA_SUCCESS) {
        fprintf(stderr, "Audio: Failed to start miniaudio engine/device (error %d)\n", startResult);
        BSC_debugLog("audio-error", "ma_engine_start failed result=%d", (int)startResult);
    }

    // Slots were already cleared by resetForGameChange. At process start they
    // are zero from calloc. Do not wipe active instances here: some games call
    // audio init helpers immediately after Runner_create and then expect them
    // to survive through room setup.
    ma->nextInstanceCounter = 0;
    ma->autoStartMusicStreamsAfterGameChange = false;

    fprintf(stderr, "Audio: miniaudio engine initialized\n");
    BSC_debugLog("audio-init", "miniaudio engine ready started=%d afterGameChange=%d groups=%d", (int)startResult, ma->afterGameChange ? 1 : 0, (int)arrlen(ma->base.audioGroups));
}

static void maDestroy(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Uninit all active sound instances
    repeat(MAX_SOUND_INSTANCES, i) {
        if (ma->instances[i].active) {
            cleanupSoundInstance(&ma->instances[i]);
        }
    }

    // Free stream entries
    repeat(MAX_AUDIO_STREAMS, i) {
        if (ma->streams[i].active) {
            free(ma->streams[i].filePath);
        }
    }

    // Free loaded audio groups. The main data.win is owned by the caller, so skip index 0.
    if (arrlen(ma->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(ma->base.audioGroups); i++) {
            DataWin_free(ma->base.audioGroups[i]);
        }
    }
    arrfree(ma->base.audioGroups);

    if (ma->engineInitialized) {
        ma_engine_stop(&ma->engine);
        ma_engine_uninit(&ma->engine);
        ma->engineInitialized = false;
    }
    free(ma);
}

static void maResetForGameChange(AudioSystem* audio, DataWin* newDataWin, FileSystem* fileSystem) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    if (ma == nullptr) return;

    fprintf(stderr, "Audio: preserving miniaudio device, clearing all chapter audio for game_change\n");
    BSC_debugLog("audio-reset", "preserve device; clear sounds/streams/groups for game_change");

    // Stop the live mixer before destroying any ma_sound that the callback may
    // currently be reading. This prevents the heap overflows/asserts we saw in
    // ma_engine_node_process_pcm_frames__sound, but avoids recreating the audio
    // device, which caused total silence after game_change on Linux.
    if (ma->engineInitialized) {
        ma_engine_stop(&ma->engine);
    }

    repeat(MAX_SOUND_INSTANCES, i) {
        if (ma->instances[i].active) {
            ma_sound_stop(&ma->instances[i].maSound);
            cleanupSoundInstance(&ma->instances[i]);
        }
    }
    memset(ma->instances, 0, sizeof(ma->instances));

    repeat(MAX_AUDIO_STREAMS, i) {
        if (ma->streams[i].active) {
            free(ma->streams[i].filePath);
        }
    }
    memset(ma->streams, 0, sizeof(ma->streams));

    // Free old auxiliary audio groups, then clear the whole table. Runner_create
    // will add the newly-loaded chapter data.win as group 0.
    if (arrlen(ma->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(ma->base.audioGroups); i++) {
            DataWin_free(ma->base.audioGroups[i]);
        }
    }
    arrfree(ma->base.audioGroups);
    ma->base.audioGroups = nullptr;

    ma->fileSystem = fileSystem;
    ma->nextInstanceCounter = 0;
    ma->afterGameChange = true;
    ma->autoStartMusicStreamsAfterGameChange = false;

    if (ma->engineInitialized) {
        ma_engine_set_volume(&ma->engine, 1.0f);
        ma_result startResult = ma_engine_start(&ma->engine);
        BSC_debugLog("audio-reset", "engine restarted after clear result=%d", (int)startResult);
    }
}


static void maUpdate(AudioSystem* audio, float deltaTime) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (!inst->active) continue;

        // Handle gain fading (for cases where we do manual fading)
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (0.0f >= inst->fadeTimeRemaining) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            ma_sound_set_volume(&inst->maSound, inst->currentGain);
        }

        // Clean up ended non-looping sounds (ma_sound_at_end avoids reaping still-loading async sounds)
        if (ma_sound_at_end(&inst->maSound) && !ma_sound_is_looping(&inst->maSound)) {
            cleanupSoundInstance(inst);
        }
    }
}

static int32_t maPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Check if this is a stream index (created by audio_create_stream)
    bool isStream = (soundIndex >= AUDIO_STREAM_INDEX_BASE);
    Sound* sound = nullptr;
    char* streamPath = nullptr;

    if (isStream) {
        int32_t streamSlot = soundIndex - AUDIO_STREAM_INDEX_BASE;
        if (0 > streamSlot || streamSlot >= MAX_AUDIO_STREAMS || !ma->streams[streamSlot].active) {
            fprintf(stderr, "Audio: Invalid stream index %d\n", soundIndex);
            return -1;
        }
        streamPath = ma->streams[streamSlot].filePath;
    } else {
        DataWin* dw = ma->base.audioGroups[0]; // Audio Group 0 should always be data.win
        if (0 > soundIndex || (uint32_t) soundIndex >= dw->sond.count) {
            fprintf(stderr, "Audio: Invalid sound index %d\n", soundIndex);
            return -1;
        }
        sound = &dw->sond.sounds[soundIndex];
    }

    if (isStream) {
        SoundInstance* existing = findActiveInstanceForSound(ma, soundIndex);
        if (existing != nullptr) {
            if (!ma_sound_is_playing(&existing->maSound)) {
                ma_sound_start(&existing->maSound);
            }
            BSC_debugLog("audio-play", "reuse existing stream instance=%d soundIndex=%d", existing->instanceId, soundIndex);
            return existing->instanceId;
        }
    }

    SoundInstance* slot = findFreeSlot(ma);
    if (slot == nullptr) {
        fprintf(stderr, "Audio: No free sound slots for sound %d\n", soundIndex);
        return -1;
    }

    int32_t slotIndex = (int32_t) (slot - ma->instances);
    ma_result result;

    if (isStream) {
        // DELTARUNE chapter switches can hit a miniaudio/stb_vorbis streaming
        // assertion during the first frames after an in-process data.win swap.
        // Load external OGG streams synchronously/decoded instead. This is also
        // friendlier for PS2-style ports where predictable IO beats async stream
        // callbacks during loading.
        result = initFileSound(ma, slot, streamPath, true);
        if (result != MA_SUCCESS) {
            fprintf(stderr, "Audio: Failed to fully decode stream file '%s' (error %d)\n", streamPath, result);
            return -1;
        }
    } else {
        bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
        bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
        bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
        bool inAudo = !isRegular || isEmbedded || isCompressed;

        if (inAudo) {
            // Embedded audio: decode from AUDO chunk memory
            if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) {
                fprintf(stderr, "Audio: Invalid audio file index %d for sound '%s'\n", sound->audioFile, sound->name);
                return -1;
            }

            AudioEntry* entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];

            ma_decoder_config decoderConfig = ma_decoder_config_init_default();
            result = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &slot->decoder);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to init decoder for '%s' (error %d)\n", sound->name, result);
                return -1;
            }
            slot->ownsDecoder = true;

            result = ma_sound_init_from_data_source(&ma->engine, &slot->decoder, 0, nullptr, &slot->maSound);
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to init sound from decoder for '%s' (error %d)\n", sound->name, result);
                ma_decoder_uninit(&slot->decoder);
                return -1;
            }
        } else {
            // External audio: load from file
            char* path = resolveExternalPath(ma, sound);
            if (path == nullptr) {
                fprintf(stderr, "Audio: Could not resolve path for sound '%s'\n", sound->name);
                return -1;
            }

            // Fully decode external OGGs too. After an in-process game_change,
            // miniaudio's streaming file backend can leave the new chapter silent
            // or trip assertions while the old chapter was just torn down.
            // Decoded buffers are stable across chapter loads and are PS2-friendlier.
            result = initFileSound(ma, slot, path, isLikelyMusicStreamPath(path));
            if (result != MA_SUCCESS) {
                fprintf(stderr, "Audio: Failed to fully decode file for '%s' at '%s' (error %d)\n", sound->name, path, result);
                free(path);
                return -1;
            }
            free(path);
        }
    }

    // Apply properties
    float volume = isStream ? 1.0f : sound->volume;
    float pitch = isStream ? 1.0f : sound->pitch;
    ma_sound_set_volume(&slot->maSound, volume);
    if (!(pitch > 0.0f) || pitch != pitch) {
        pitch = 1.0f;
    }
    if (pitch != 1.0f) {
        ma_sound_set_pitch(&slot->maSound, pitch);
    }
    ma_sound_set_looping(&slot->maSound, loop);

    // Set up instance tracking
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->instanceId = SOUND_INSTANCE_ID_BASE + slotIndex;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->startGain = volume;
    slot->priority = priority;

    // Track unique IDs for disambiguation
    ma->nextInstanceCounter++;

    if (ma->engineInitialized) {
        ma_result engStart = ma_engine_start(&ma->engine);
        if (engStart != MA_SUCCESS) {
            BSC_debugLog("audio-error", "ma_engine_start before play failed result=%d", (int)engStart);
        }
    }

    result = ma_sound_start(&slot->maSound);
    if (result != MA_SUCCESS) {
        BSC_debugLog("audio-error", "ma_sound_start failed result=%d soundIndex=%d", (int)result, soundIndex);
        cleanupSoundInstance(slot);
        return -1;
    }
    fprintf(stderr, "Audio: Started instance %d for sound %d (stream=%d loop=%d)\n", slot->instanceId, soundIndex, isStream ? 1 : 0, loop ? 1 : 0);
    BSC_debugLog("audio-play", "started instance=%d soundIndex=%d stream=%d loop=%d", slot->instanceId, soundIndex, isStream ? 1 : 0, loop ? 1 : 0);

    return slot->instanceId;
}

static void maStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        // Stop specific instance
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
            cleanupSoundInstance(inst);
        }
    } else {
        // Stop all instances of this sound resource
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
                cleanupSoundInstance(inst);
            }
        }
    }
}

static void maStopAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            ma_sound_stop(&inst->maSound);
            cleanupSoundInstance(inst);
        }
    }
}

static bool maIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        return inst != nullptr && ma_sound_is_playing(&inst->maSound);
    } else {
        // Check if any instance of this sound resource is playing
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance && ma_sound_is_playing(&inst->maSound)) {
                return true;
            }
        }
        return false;
    }
}

static void maPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_stop(&inst->maSound);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_stop(&inst->maSound);
            }
        }
    }
}

static void maResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_start(&inst->maSound);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_start(&inst->maSound);
            }
        }
    }
}

static void maPauseAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && ma_sound_is_playing(&inst->maSound)) {
            ma_sound_stop(&inst->maSound);
        }
    }
}

static void maResumeAll(AudioSystem* audio) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active) {
            ma_sound_start(&inst->maSound);
        }
    }
}

static void maSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            if (timeMs == 0) {
                inst->currentGain = gain;
                inst->targetGain = gain;
                inst->fadeTimeRemaining = 0.0f;
                ma_sound_set_volume(&inst->maSound, gain);
            } else {
                inst->startGain = inst->currentGain;
                inst->targetGain = gain;
                inst->fadeTotalTime = (float) timeMs / 1000.0f;
                inst->fadeTimeRemaining = inst->fadeTotalTime;
            }
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                if (timeMs == 0) {
                    inst->currentGain = gain;
                    inst->targetGain = gain;
                    inst->fadeTimeRemaining = 0.0f;
                    ma_sound_set_volume(&inst->maSound, gain);
                } else {
                    inst->startGain = inst->currentGain;
                    inst->targetGain = gain;
                    inst->fadeTotalTime = (float) timeMs / 1000.0f;
                    inst->fadeTimeRemaining = inst->fadeTotalTime;
                }
            }
        }
    }
}

static float maGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return inst->currentGain;
            }
        }
    }
    return 0.0f;
}

static void maSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_set_pitch(&inst->maSound, pitch);
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_set_pitch(&inst->maSound, pitch);
            }
        }
    }
}

static float maGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) return ma_sound_get_pitch(&inst->maSound);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                return ma_sound_get_pitch(&inst->maSound);
            }
        }
    }
    return 1.0f;
}

static float maGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            float cursor;
            ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
            if (result == MA_SUCCESS) return cursor;
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                float cursor;
                ma_result result = ma_sound_get_cursor_in_seconds(&inst->maSound, &cursor);
                if (result == MA_SUCCESS) return cursor;
            }
        }
    }
    return 0.0f;
}

static void maSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        SoundInstance* inst = findInstanceById(ma, soundOrInstance);
        if (inst != nullptr) {
            ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64) (positionSeconds * 44100.0f));
        }
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                ma_sound_seek_to_pcm_frame(&inst->maSound, (ma_uint64) (positionSeconds * 44100.0f));
            }
        }
    }
}

// Total length of a loaded sound. Works on both SOND index and active instance ids.
// Uses miniaudio's ma_sound_get_length_in_seconds, which reads the decoded duration from the underlying data source (works for fully-decoded sounds AND streaming sounds).
static float maGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    SoundInstance* match = nullptr;
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        match = findInstanceById(ma, soundOrInstance);
    } else {
        repeat(MAX_SOUND_INSTANCES, i) {
            SoundInstance* inst = &ma->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) {
                match = inst;
                break;
            }
        }
    }
    if (match != nullptr) {
        float seconds = 0.0f;
        if (ma_sound_get_length_in_seconds(&match->maSound, &seconds) != MA_SUCCESS) return 0.0f;
        return seconds;
    }

    // No active instance: GMS audio_sound_length(soundIndex) must still return the asset's duration.
    if (soundOrInstance >= SOUND_INSTANCE_ID_BASE || soundOrInstance >= AUDIO_STREAM_INDEX_BASE)
        return 0.0f;

    DataWin* dw = ma->base.audioGroups[0];
    if (dw == nullptr || 0 > soundOrInstance || (uint32_t) soundOrInstance >= dw->sond.count)
        return 0.0f;

    Sound* sound = &dw->sond.sounds[soundOrInstance];

    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;
    ma_decoder decoder;
    ma_result decResult;
    if (inAudo) {
        if (0 > sound->audioFile || (uint32_t) sound->audioFile >= ma->base.audioGroups[sound->audioGroup]->audo.count) return 0.0f;
        AudioEntry* entry = &ma->base.audioGroups[sound->audioGroup]->audo.entries[sound->audioFile];
        ma_decoder_config decoderConfig = ma_decoder_config_init_default();
        decResult = ma_decoder_init_memory(entry->data, entry->dataSize, &decoderConfig, &decoder);
    } else {
        char* path = resolveExternalPath(ma, sound);
        if (path == nullptr) return 0.0f;
        ma_decoder_config decoderConfig = ma_decoder_config_init_default();
        decResult = ma_decoder_init_file(path, &decoderConfig, &decoder);
        free(path);
    }
    if (decResult != MA_SUCCESS) return 0.0f;

    ma_uint64 frames = 0;
    float seconds = 0.0f;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS) {
        ma_uint32 sampleRate = decoder.outputSampleRate;
        if (sampleRate > 0) seconds = (float) frames / (float) sampleRate;
    }
    ma_decoder_uninit(&decoder);
    return seconds;
}

static void maSetMasterGain(AudioSystem* audio, float gain) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;
    if (!ma->engineInitialized) {
        BSC_debugLog("audio-gain", "ignored master gain while engine stopped gain=%f", gain);
        return;
    }
    ma_engine_set_volume(&ma->engine, gain);
    BSC_debugLog("audio-gain", "master gain=%f", gain);
}

static void maSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {
    // miniaudio handles channel management internally, this is a no-op
}

static void maGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex > 0) {
        int sz = snprintf(nullptr, 0, "audiogroup%d.dat", groupIndex);
        char buf[sz + 1];
        snprintf(buf, sizeof(buf), "audiogroup%d.dat", groupIndex);

        // The original runner does not care if the file doesn't exist (this may happen if someone uses "audio_group_load" on a non-existent group)
        FileSystem* fileSystem = ((MaAudioSystem*)audio)->fileSystem;
        char* resolvedPath = (((MaAudioSystem*)audio)->fileSystem->vtable->resolvePath(((MaAudioSystem*)audio)->fileSystem, buf));
        if (!fileSystem->vtable->fileExists(fileSystem, resolvedPath)) {
            fprintf(stderr, "Audio: Wanted to load Audio Group %d, but Audio Group %d does not exist!\n", groupIndex, groupIndex);
            return;
        }

        DataWin *audioGroup = DataWin_parse(((MaAudioSystem*)audio)->fileSystem->vtable->resolvePath(((MaAudioSystem*)audio)->fileSystem, buf),
        (DataWinParserOptions) {
            .parseAudo = true,
        });
        arrput(audio->audioGroups, audioGroup);
    }
}

static bool maGroupIsLoaded(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t groupIndex) {
    return (arrlen(audio->audioGroups) > groupIndex);
}

// ===[ Audio Streams ]===

static int32_t maCreateStream(AudioSystem* audio, const char* filename) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    // Find a free stream slot
    int32_t freeSlot = -1;
    repeat(MAX_AUDIO_STREAMS, i) {
        if (!ma->streams[i].active) {
            freeSlot = (int32_t) i;
            break;
        }
    }

    if (0 > freeSlot) {
        fprintf(stderr, "Audio: No free stream slots for '%s'\n", filename);
        BSC_debugLog("audio-error", "no free stream slots filename=%s", filename);
        return -1;
    }

    char* resolved = ma->fileSystem->vtable->resolvePath(ma->fileSystem, filename);
    if (resolved == nullptr) {
        fprintf(stderr, "Audio: Could not resolve path for stream '%s'\n", filename);
        BSC_debugLog("audio-error", "could not resolve stream filename=%s", filename);
        return -1;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        fprintf(stderr, "Audio: Stream file does not exist for '%s' -> '%s'\n", filename, resolved);
        BSC_debugLog("audio-error", "missing stream filename=%s resolved=%s", filename, resolved);
        free(resolved);
        return -1;
    }

    ma->streams[freeSlot].active = true;
    ma->streams[freeSlot].filePath = resolved;

    int32_t streamIndex = AUDIO_STREAM_INDEX_BASE + freeSlot;
    fprintf(stderr, "Audio: Created stream %d for '%s' -> '%s'\n", streamIndex, filename, resolved);
    BSC_debugLog("audio-stream", "created index=%d filename=%s resolved=%s", streamIndex, filename, resolved);

    // Do not auto-start streams. DELTARUNE scripts intentionally choose whether a stream loops.

    return streamIndex;
}

static bool maDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    MaAudioSystem* ma = (MaAudioSystem*) audio;

    int32_t slotIndex = streamIndex - AUDIO_STREAM_INDEX_BASE;
    if (0 > slotIndex || slotIndex >= MAX_AUDIO_STREAMS) {
        // Deltarune's snd_free_all loops a larger GameMaker stream-id range
        // than this backend stores. Treat out-of-range destroys as harmless.
        return false;
    }

    AudioStreamEntry* entry = &ma->streams[slotIndex];
    if (!entry->active) return false;

    // Stop all sound instances that were playing this stream
    repeat(MAX_SOUND_INSTANCES, i) {
        SoundInstance* inst = &ma->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) {
            ma_sound_stop(&inst->maSound);
            cleanupSoundInstance(inst);
        }
    }

    free(entry->filePath);
    entry->filePath = nullptr;
    entry->active = false;
    fprintf(stderr, "Audio: Destroyed stream %d\n", streamIndex);
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable maAudioSystemVtable = {
    .init = maInit,
    .destroy = maDestroy,
    .update = maUpdate,
    .playSound = maPlaySound,
    .stopSound = maStopSound,
    .stopAll = maStopAll,
    .isPlaying = maIsPlaying,
    .pauseSound = maPauseSound,
    .resumeSound = maResumeSound,
    .pauseAll = maPauseAll,
    .resumeAll = maResumeAll,
    .setSoundGain = maSetSoundGain,
    .getSoundGain = maGetSoundGain,
    .setSoundPitch = maSetSoundPitch,
    .getSoundPitch = maGetSoundPitch,
    .getTrackPosition = maGetTrackPosition,
    .setTrackPosition = maSetTrackPosition,
    .getSoundLength = maGetSoundLength,
    .setMasterGain = maSetMasterGain,
    .setChannelCount = maSetChannelCount,
    .groupLoad = maGroupLoad,
    .groupIsLoaded = maGroupIsLoaded,
    .createStream = maCreateStream,
    .destroyStream = maDestroyStream,
    .resetForGameChange = maResetForGameChange,
};

// ===[ Lifecycle ]===

MaAudioSystem* MaAudioSystem_create(void) {
    MaAudioSystem* ma = safeCalloc(1, sizeof(MaAudioSystem));
    ma->base.vtable = &maAudioSystemVtable;
    return ma;
}
