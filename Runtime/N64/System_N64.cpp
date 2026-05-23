/**
 * @file System_N64.cpp
 * @brief N64-side (libdragon) implementation of the engine's SYS_* surface.
 *
 * Mirrors System_PSP.cpp structurally. N64 specifics:
 *
 *   - File I/O is standard POSIX (libdragon's newlib maps fopen/fread to
 *     DragonFS when the path is under `rom:/...` and to the cartridge SD/
 *     CF card driver when under `sd:/...`).
 *   - Directory iteration uses opendir/readdir/closedir on DragonFS.
 *   - Threading is STUBBED single-threaded for Phase 1 — libdragon's
 *     `kthread_*` API can be wired up later. Engine workers run inline
 *     today; this is a known Phase 1 limitation.
 *   - Mutexes are no-op stubs for the same reason.
 *   - Time is `get_ticks_us()` from libdragon (cop0 count register).
 *   - No window concepts (320×240 fixed); window/clipboard SYS_ are
 *     no-op stubs.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "System/System.h"
#include "Engine.h"
#include "Stream.h"
#include "Log.h"
#include "Utilities.h"

#include <libdragon.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <string>

// libdragon's newlib explicitly errors on <dirent.h> — DragonFS uses its
// own dir_t / dir_findfirst / dir_findnext API instead. For Phase 1 we
// don't iterate directories anywhere user-visible (assets ship via the
// embedded-scripts table; no asset registry is loaded from disk), so all
// the directory SYS_* functions below are stubs. A later phase wires up
// the libdragon DragonFS dir API for real iteration over `rom:/...`.

static bool sInitialized = false;

// =========================================================================
// Lifecycle
// =========================================================================

void SYS_Initialize()
{
    if (sInitialized) return;

    // Display + interrupts + DragonFS are all initialised in Main_N64.cpp
    // BEFORE the engine boots, so SYS_Initialize is mostly free of side
    // effects. Anything that depends on EngineState being live (clock
    // tracking, OS-style services) lands here later.

    sInitialized = true;
    LogDebug("System_N64: initialised");
    debugf("[SYS] SYS_Initialize returning\n");
}

void SYS_Shutdown()
{
    // Main_N64.cpp handles libdragon teardown; engine shutdown just flips
    // the flag.
    sInitialized = false;
}

void SYS_Update()
{
    // libdragon's interrupt-driven services (timers, controllers, audio)
    // are fully async — no per-frame pump needed here.
}

// =========================================================================
// Paths
// =========================================================================

std::string SYS_GetExecutablePath()
{
    // N64 has no executable-path concept beyond the ROM itself. Return a
    // synthetic path for logging consistency.
    return "rom:/Polyphase.z64";
}

std::string SYS_GetPolyphasePath()
{
    // Engine-bundled assets live in DragonFS under `rom:/`.
    return "rom:/";
}

std::string SYS_GetCurrentDirectoryPath()
{
    return "rom:/";
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    if (relativePath.size() >= 4 && relativePath.compare(0, 4, "rom:") == 0) return relativePath;
    if (relativePath.size() >= 3 && relativePath.compare(0, 3, "sd:") == 0)  return relativePath;
    return SYS_GetPolyphasePath() + relativePath;
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}
void SYS_SetWorkingDirectory(const std::string& /*dirPath*/) {}

// =========================================================================
// File I/O — POSIX-style via libdragon's newlib. DragonFS is read-only,
// so SYS_CreateDirectory / RemoveDirectory / CopyFile etc. operate on the
// SD card driver if mounted (libdragon flashcart support).
// =========================================================================

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void SYS_AcquireFileData(const char* path, bool /*isAsset*/, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        LogWarning("SYS_AcquireFileData: fopen failed for '%s'", path);
        return;
    }

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size < 0) { std::fclose(f); return; }

    uint32_t actualSize = (uint32_t)size;
    if (maxSize > 0 && actualSize > (uint32_t)maxSize) actualSize = (uint32_t)maxSize;

    outData = (char*)std::malloc(actualSize);
    if (outData == nullptr)
    {
        std::fclose(f);
        LogError("SYS_AcquireFileData: malloc(%u) failed for '%s'", actualSize, path);
        return;
    }

    const size_t read = std::fread(outData, 1, actualSize, f);
    std::fclose(f);

    if (read != actualSize)
    {
        // Short read is non-fatal — DragonFS streaming over PI can return
        // partial reads at EOF; cap the visible size to what we got.
        actualSize = (uint32_t)read;
    }
    outSize = actualSize;
}

void SYS_ReleaseFileData(char* data)
{
    std::free(data);
}

bool SYS_CreateDirectory(const char* dirPath)
{
    // DragonFS is read-only. SD card via libdragon's flashcart bridge would
    // support mkdir — for now treat as unsupported.
    (void)dirPath;
    return false;
}

void SYS_RemoveDirectory(const char* dirPath)
{
    (void)dirPath;
}

// Phase 1 stubs. libdragon's newlib has no <dirent.h>; DragonFS uses its
// own dir_findfirst / dir_findnext API. Engine asset discovery on N64
// runs from embedded scripts only at this stage, so a no-op iterator is
// sufficient — every call to SYS_OpenDirectory immediately reports an
// invalid entry and the engine's discovery loop bails cleanly.
void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    debugf("[SYS] SYS_OpenDirectory('%s') -> stub invalid\n", dirPath.c_str());
    outDirEntry.mValid     = false;
    outDirEntry.mDirHandle = nullptr;
    outDirEntry.mFilename[0]      = '\0';
    outDirEntry.mDirectoryPath[0] = '\0';
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    dirEntry.mValid = false;
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    dirEntry.mDirHandle = nullptr;
    dirEntry.mValid     = false;
}

void SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return;

    FILE* src = std::fopen(sourcePath, "rb");
    if (src == nullptr) return;
    FILE* dst = std::fopen(destPath, "wb");
    if (dst == nullptr) { std::fclose(src); return; }

    char buf[1024];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), src)) > 0)
    {
        if (std::fwrite(buf, 1, n, dst) != n) break;
    }
    std::fclose(src);
    std::fclose(dst);
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
void SYS_MoveDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
void SYS_MoveFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath && destPath) std::rename(sourcePath, destPath);
}
void SYS_RemoveFile(const char* path)
{
    if (path) std::remove(path);
}
bool SYS_Rename(const char* oldPath, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr) return false;
    return std::rename(oldPath, newPath) == 0;
}

std::vector<std::string> SYS_OpenFileDialog() { return {}; }
std::string SYS_SaveFileDialog() { return ""; }
std::string SYS_SelectFolderDialog() { return ""; }

std::string SYS_GetFileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// =========================================================================
// Threading — Phase 1 SINGLE-THREADED stub.
//
// libdragon ships a kernel threading API (kthread_*) in V2, but Polyphase's
// engine works single-threaded just fine on PSP/3DS — worker threads are
// performance, not correctness. For Phase 1 we run the entry function
// inline on the main thread so anything that expects a thread to "exist"
// gets a valid (single-shot) execution; Join and Destroy are then no-ops.
//
// Future phase: rewrite using libdragon's kthread_t / kmutex_t for real
// concurrency on the audio + asset-loader paths.
// =========================================================================

namespace
{
    struct N64Thread
    {
        bool mFinished;
    };
}

ThreadObject* SYS_CreateThread(ThreadFuncFP func, void* arg)
{
    (void)func;
    (void)arg;
    // Phase 1 cannot run threads. Earlier this stub ran the callable
    // INLINE on the assumption that worker functions were one-shot ("do
    // some work and return"), but engine workers are actually long-lived
    // loops (`while (running) { wait_for_work(); }`) — calling them
    // synchronously infinite-loops the engine boot. AssetManager's
    // AsyncLoadThreadFunc was the canonical case (hung the boot right
    // after SYS_Initialize returned).
    //
    // The right behaviour on a single-threaded console is to silently
    // skip thread creation. Callers see a non-null handle (so subsequent
    // Join/Destroy paths don't blow up), but the worker function never
    // runs. Engine subsystems that need the worker degrade to "feature
    // disabled" — async asset loading falls back to synchronous loads,
    // background prefetch is a no-op, etc. Phase 1 is correct; perf is
    // a follow-up.
    ThreadObject* out = new ThreadObject;
    *out = 1; // non-zero "valid" sentinel
    return out;
}

void SYS_JoinThread(ThreadObject* /*thread*/) {}

void SYS_DestroyThread(ThreadObject* thread)
{
    delete thread;
}

MutexObject* SYS_CreateMutex()
{
    MutexObject* out = new MutexObject;
    *out = 1;
    return out;
}

void SYS_LockMutex(MutexObject* /*mutex*/) {}
void SYS_UnlockMutex(MutexObject* /*mutex*/) {}
void SYS_DestroyMutex(MutexObject* mutex)
{
    delete mutex;
}

void SYS_Sleep(uint32_t milliseconds)
{
    // wait_ms is libdragon's busy-wait primitive — fine on Phase 1 single-
    // threaded boot. Future phase swaps for a real yield via kthread.
    ::wait_ms(milliseconds);
}

// =========================================================================
// Time — libdragon's get_ticks() returns COP0 count register (46.875 MHz).
// TICKS_TO_MS / TICKS_TO_US macros are provided by libdragon. We compute
// in microseconds explicitly to match SYS_GetTimeMicroseconds' contract.
// =========================================================================

uint64_t SYS_GetTimeMicroseconds()
{
    // get_ticks() / get_ticks_ms() / get_ticks_us() — use the µs variant
    // when available; otherwise compute manually. libdragon's
    // TICKS_PER_SECOND is 93750000 / 2 = 46875000 (half the CPU clock).
    // (uint64_t)ticks * 1000000ULL / 46875000ULL.
    const uint64_t ticks = (uint64_t)::get_ticks();
    return ticks * 1000000ULL / 46875000ULL;
}

// =========================================================================
// Process exec — N/A.
// =========================================================================

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// =========================================================================
// Memory
// =========================================================================

// libdragon's <malloc.h> doesn't expose memalign even though newlib provides
// the symbol. Forward-declare here so the call links without the header
// declaration. (posix_memalign is the alternative, but it has a different
// signature — *outPtr first — which would require restructuring the call.)
extern "C" void* memalign(size_t alignment, size_t size);

void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    std::free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;

    MemoryStat mainRam;
    mainRam.mName = "MainRAM";
    // libdragon's heap_stats() returns total/used bytes — fall back to
    // a coarse estimate when not available. Polyphase doesn't currently
    // require accurate values; SYS_GetTotalRAM is the load-bearing one
    // for the budget tooling.
    mainRam.mBytesAllocated = 0;
    mainRam.mBytesFree = GetEngineState() && GetEngineState()->mSystem.mExpansionPak
                       ? (8u * 1024u * 1024u)
                       : (4u * 1024u * 1024u);
    stats.push_back(mainRam);

    return stats;
}

float SYS_GetRAMUsage()    { return 0.0f; }
float SYS_GetVRAMUsage()   { return 0.0f; }
float SYS_GetRAM1Usage()   { return 0.0f; }
float SYS_GetRAM2Usage()   { return 0.0f; }
float SYS_GetCPUUsage()    { return 0.0f; }
float SYS_GetTotalRAM()
{
    // Stock N64: 4 MB. With Expansion Pak: 8 MB. Engine code checks
    // SystemState::mExpansionPak (populated by Main_N64 based on the
    // N64_EXPANSION_PAK define and runtime detection).
    return (GetEngineState() && GetEngineState()->mSystem.mExpansionPak)
        ? (float)(8u * 1024u * 1024u)
        : (float)(4u * 1024u * 1024u);
}
float SYS_GetTotalVRAM()   { return 0.0f; }
float SYS_GetTotalRAM1()   { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()   { return 0.0f; }

// =========================================================================
// Save data — controller pak / cart SRAM. Phase 1 stubs these out; a
// future phase wires libdragon's controller-pak / eeprom / sram APIs.
// =========================================================================

bool SYS_ReadSave(const char* /*saveName*/, Stream& /*outStream*/) { return false; }
bool SYS_WriteSave(const char* /*saveName*/, Stream& /*stream*/)   { return false; }
bool SYS_DoesSaveExist(const char* /*saveName*/)                   { return false; }
bool SYS_DeleteSave(const char* /*saveName*/)                      { return false; }
void SYS_UnmountMemoryCard() {}

// =========================================================================
// Clipboard — N/A
// =========================================================================

void SYS_SetClipboardText(const std::string& /*str*/) {}
std::string SYS_GetClipboardText() { return ""; }

// =========================================================================
// Logging / assertions / dialogs
// =========================================================================

// libdragon's debugf() routes to ISViewer (Mupen + ares can capture it) AND
// to the UART if usb logging is initialised in Main_N64. Use it directly
// from SYS_Log so log lines surface in both emulator log windows and the
// USB serial on real hardware via flashcarts that expose UART.
void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), format, arg);

    const char* sevTag = (severity == LogSeverity::Error)   ? "[E] "
                       : (severity == LogSeverity::Warning) ? "[W] "
                       :                                       "[D] ";

    ::debugf("%s%s\n", sevTag, buf);
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    ::debugf("ASSERT: %s at %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    ::abort();
}

void SYS_Alert(const char* message)
{
    ::debugf("ALERT: %s\n", message);
}

void SYS_UpdateConsole() {}

int32_t SYS_GetPlatformTier()
{
    // 0 = stock 4 MB, 1 = Expansion Pak 8 MB. Read from EngineState which
    // Main_N64 populates after libdragon detects the pak.
    return (GetEngineState() && GetEngineState()->mSystem.mExpansionPak) ? 1 : 0;
}

// =========================================================================
// Window — N/A on N64 (fixed 320×240 framebuffer)
// =========================================================================

void SYS_SetWindowTitle(const char* /*title*/) {}
void SYS_SetWindowIcon(const char* /*iconPath*/) {}
bool SYS_DoesWindowHaveFocus() { return true; }
void SYS_SetScreenOrientation(ScreenOrientation /*orientation*/) {}
ScreenOrientation SYS_GetScreenOrientation() { return ScreenOrientation::Landscape; }
void SYS_SetFullscreen(bool /*fullscreen*/) {}
bool SYS_IsFullscreen() { return true; }
void SYS_SetWindowRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
void SYS_GetWindowRect(int32_t& outX, int32_t& outY, int32_t& outWidth, int32_t& outHeight)
{
    outX = 0;
    outY = 0;
    outWidth  = 320;
    outHeight = 240;
}
bool SYS_IsWindowMaximized() { return true; }
void SYS_MaximizeWindow() {}

#endif // POLYPHASE_PLATFORM_ADDON
