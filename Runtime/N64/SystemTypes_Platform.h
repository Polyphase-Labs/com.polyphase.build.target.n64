/**
 * @file SystemTypes_Platform.h
 * @brief N64 platform extension for the engine's `SystemTypes.h` fork.
 *
 * Picked up automatically by the engine when `POLYPHASE_PLATFORM_ADDON=1`
 * is defined at compile time. ActionManager generates a bridge header at
 * `<projectDir>/Generated/PolyphasePlatform_SystemTypes.h` that includes
 * this file via absolute path. Makefile_N64 puts Generated/ on the include
 * path.
 *
 * We intentionally avoid including libdragon's `<libdragon.h>` here — that
 * header pulls in the entire RDP/RSP/audio/controller stack, drags symbols
 * into every engine translation unit, and is fragile under -fno-exceptions.
 * Instead we use opaque integer handles + raw byte storage for DirEntry's
 * directory state; System_N64.cpp casts them back to their real
 * libdragon-side types (FILE* / struct dirent) at the impl boundary.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// ----- Threading typedefs --------------------------------------------------
// libdragon's kernel/thread API uses opaque handles; we model them as 32-bit
// ids. Thread entry functions return `int` (matching POSIX-ish convention);
// we do NOT define POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN so
// THREAD_RETURN() resolves to `return 0;` (the engine default branch).
typedef int32_t  ThreadObject;
typedef int32_t  MutexObject;
typedef int      ThreadFuncRet;

// ----- DirEntry injection --------------------------------------------------
// DragonFS exposes ROM contents through a POSIX dirent-style API (opendir /
// readdir / closedir under `rom:/...`). The handle and last-read name are
// stashed here; the engine's directory iteration logic uses opaque pointers
// and the size of the name buffer follows libdragon's DRAGONFS_MAX_PATHLEN
// (256, matching standard newlib).
#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    void*    mDirHandle = nullptr; /* DIR* — kept opaque to avoid pulling <dirent.h> here */ \
    char     mLastName[256] = {0};

// ----- SystemState injection -----------------------------------------------
// N64 is fixed-hardware — no concept of a "window" beyond the framebuffer
// size set by display_init. We track a quit flag (the engine's main loop
// polls it) plus identifiers the engine can surface in diagnostics.
//
// N64 CPU clock: 93.75 MHz (VR4300); RSP clock: 62.5 MHz. RDP shares the
// RSP/RAM bus; there's no GPU clock per se. Values are reported in MHz so
// the engine's existing helpers don't need new units.
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool     mQuitRequested  = false; \
    bool     mExpansionPak   = false; \
    int32_t  mCpuClockMhz    = 93;  /* VR4300 */ \
    int32_t  mRspClockMhz    = 62;  /* RSP coprocessor */
