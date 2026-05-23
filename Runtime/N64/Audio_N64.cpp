/**
 * @file Audio_N64.cpp
 * @brief N64 audio — Phase 1 silent stub.
 *
 * Phase 1: all AUD_* functions are no-op stubs that keep the engine
 * linking. The engine's audio paths (sound emitter nodes, music players,
 * video-player streaming, etc.) all degrade gracefully when AUD_IsPlaying
 * returns false consistently.
 *
 * Phase 4: real implementation via libdragon's audio_init + mixer.
 * libdragon's mixer ships out of the box with the SDK and uses N64's
 * AI (audio interface) with a pull-style buffer queue. The shape would
 * mirror Audio_PSP.cpp — a software mixer with a voice table, drained
 * each frame inside AUD_Update.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <libdragon.h>  // debugf macro

#include <cstdint>
#include <cstdlib>

void AUD_Initialize()
{
    debugf("[AUD] Entered AUD_Initialize (silent stub)\n");
    LogDebug("Audio_N64: silent stub (Phase 1)");
}

void AUD_Shutdown() {}
void AUD_Update() {}

void AUD_Play(uint32_t /*voiceIndex*/,
              SoundWave* /*soundWave*/,
              float /*volume*/,
              float /*pitch*/,
              bool /*loop*/,
              float /*startTime*/,
              bool /*spatial*/)
{
}

void AUD_Stop(uint32_t /*voiceIndex*/) {}
bool AUD_IsPlaying(uint32_t /*voiceIndex*/) { return false; }
void AUD_SetVolume(uint32_t /*voiceIndex*/, float /*leftVolume*/, float /*rightVolume*/) {}
void AUD_SetPitch(uint32_t /*voiceIndex*/, float /*pitch*/) {}

uint8_t* AUD_AllocWaveBuffer(uint32_t size)
{
    // Engine callers expect a heap allocation matching free-by-AUD_FreeWaveBuffer.
    // Use malloc directly (not memalign) — Phase 4's mixer will rewrite.
    return (uint8_t*)std::malloc(size);
}

void AUD_FreeWaveBuffer(void* buffer)
{
    std::free(buffer);
}

void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

uint32_t AUD_OpenStream(uint32_t /*sampleRate*/, uint32_t /*numChannels*/, uint32_t /*bitsPerSample*/)
{
    return 0; // signal "open failed" so video-player consumers fall back to silence
}

void     AUD_CloseStream(uint32_t /*streamId*/) {}
int32_t  AUD_SubmitStreamBuffer(uint32_t /*streamId*/, const uint8_t* /*data*/, uint32_t /*byteSize*/)
{
    return 0; // 0 = "queue full, retry" — caller will keep trying, harmless.
}
uint64_t AUD_GetStreamPlayedSamples(uint32_t /*streamId*/) { return 0; }
void     AUD_SetStreamVolume(uint32_t /*streamId*/, float /*volume*/) {}
void     AUD_SetStreamPaused(uint32_t /*streamId*/, bool /*paused*/) {}
void     AUD_FlushStream(uint32_t /*streamId*/) {}

#endif // POLYPHASE_PLATFORM_ADDON
