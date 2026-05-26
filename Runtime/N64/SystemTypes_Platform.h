// N64 platform extension for the engine's SystemTypes.h fork. Avoid
// including <libdragon.h> here; system-side casts back to libdragon types
// at the impl boundary so the rest of the engine stays insulated.

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int32_t  ThreadObject;
typedef int32_t  MutexObject;
typedef int      ThreadFuncRet;

#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    void*    mDirHandle = nullptr; \
    char     mLastName[256] = {0};

// VR4300 = 93.75 MHz, RSP = 62.5 MHz.
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool     mQuitRequested  = false; \
    bool     mExpansionPak   = false; \
    int32_t  mCpuClockMhz    = 93; \
    int32_t  mRspClockMhz    = 62;
