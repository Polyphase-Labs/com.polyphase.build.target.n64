// N64 entry point. The engine's int main is gated out when
// POLYPHASE_PLATFORM_ADDON is defined; this main is the sole entry.

#if defined(POLYPHASE_PLATFORM_ADDON)

#include <libdragon.h>

#include <cstdio>
#include <cstring>

#include "Engine.h"
#include "EmbeddedFile.h"
#include "Log.h"

extern uint32_t gNumEmbeddedScripts;
extern EmbeddedFile gEmbeddedScripts[];

// Probes whether C++ global constructors fired before main(); if this
// counter is 0 in the boot log, --gc-sections or libdragon's __do_global_ctors
// wrap is killing static init.
static volatile int sStaticInitCount = 0;
struct StaticInitProbe
{
    StaticInitProbe() { sStaticInitCount++; }
};
static StaticInitProbe sStaticInitProbe_a;
static StaticInitProbe sStaticInitProbe_b;
static StaticInitProbe sStaticInitProbe_c;

void OctPreInitialize(EngineConfig& config)
{
    GetEngineState()->mStandalone = true;

    // Force NTSC viewport — Config.ini may otherwise carry desktop dims.
    config.mWindowWidth  = 320;
    config.mWindowHeight = 240;

    config.mEmbeddedScriptCount = gNumEmbeddedScripts;
    config.mEmbeddedScripts     = gEmbeddedScripts;
}

void OctPostInitialize()
{
    // Re-clamp in case ReadEngineConfig() clobbered the dims from OctPreInitialize.
    const uint32_t beforeW = GetEngineState()->mWindowWidth;
    const uint32_t beforeH = GetEngineState()->mWindowHeight;
    GetEngineState()->mWindowWidth  = 320;
    GetEngineState()->mWindowHeight = 240;

#if defined(N64_EXPANSION_PAK)
    GetEngineState()->mSystem.mExpansionPak = true;
#else
    GetEngineState()->mSystem.mExpansionPak = (get_memory_size() >= 8 * 1024 * 1024);
#endif

    debugf("[N64] OctPostInitialize: window %ux%u -> 320x240, expansionPak=%d\n",
             (unsigned)beforeW, (unsigned)beforeH,
             (int)GetEngineState()->mSystem.mExpansionPak);
}

void OctPreUpdate() {}
void OctPostUpdate() {}
void OctPreShutdown() {}
void OctPostShutdown() {}

extern void GameMain(int32_t argc, char** argv);

int main(void)
{
    // libdragon's debug_init_* are object-like macros — do NOT prefix with
    // `::`, that breaks the macro expansion.
    debug_init_isviewer();
    debug_init_usblog();
    debugf("[1] Polyphase N64 boot start (static ctors fired: %d / expected 3)\n",
           sStaticInitCount);

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    debugf("[2] Display initialised\n");

    const int dfsRc = dfs_init(DFS_DEFAULT_LOCATION);
    if (dfsRc == DFS_ESUCCESS)
    {
        debugf("[3] DragonFS mounted\n");

        dir_t d;
        const int dfr = dir_findfirst("rom:/", &d);
        if (dfr == 0)
        {
            debugf("[3a] DragonFS root contents:\n");
            do {
                debugf("       %s%s\n", d.d_name, (d.d_type == DT_DIR) ? "/" : "");
            } while (dir_findnext("rom:/", &d) == 0);
        }
        else
        {
            debugf("[3a] dir_findfirst('rom:/') failed: %d\n", dfr);
        }

        const int dfr2 = dir_findfirst("rom:/BuildTarget-N64/", &d);
        if (dfr2 == 0)
        {
            debugf("[3b] DragonFS rom:/BuildTarget-N64/ contents:\n");
            do {
                debugf("       %s%s\n", d.d_name, (d.d_type == DT_DIR) ? "/" : "");
            } while (dir_findnext("rom:/BuildTarget-N64/", &d) == 0);
        }
        else
        {
            debugf("[3b] dir_findfirst('rom:/BuildTarget-N64/') failed: %d\n", dfr2);
        }
    }
    else
    {
        debugf("[3] DragonFS not present (rc=%d) - embedded scripts only\n", dfsRc);
    }

    controller_init();
    debugf("[4] Controllers initialised\n");

    timer_init();
    debugf("[5] Timer subsystem initialised\n");

    debugf("[6] About to call GameMain()\n");
    char  arg0[] = "polyphase";
    char* argv[] = { arg0, nullptr };
    GameMain(1, argv);
    debugf("[7] GameMain() returned cleanly\n");

    display_close();
    timer_close();

    // GameMain shouldn't return on homebrew; if it does, spin until reset.
    while (1) { wait_ms(1000); }

    return 0;
}

#endif // POLYPHASE_PLATFORM_ADDON
