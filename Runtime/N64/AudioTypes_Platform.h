/**
 * @file AudioTypes_Platform.h
 * @brief N64 platform extension for the engine's `AudioTypes.h` fork.
 *
 * Stub — Audio_N64.cpp owns the libdragon mixer state (`audio_init` +
 * `mixer_init` + a per-voice ring of `wav64_t` instances). The engine's
 * `AudioTypes.h` only forks on Windows today (XAudio2 types). No shared
 * types need to leak here yet.
 */

#pragma once
#include <stdint.h>
