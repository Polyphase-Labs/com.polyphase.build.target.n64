// libdragon-backed SYS_* surface. File I/O routes through newlib (rom:/ →
// DragonFS, sd:/ → flashcart). Threading + mutexes are single-threaded
// stubs. Time comes from cop0 via get_ticks_us. <dirent.h> isn't usable
// on libdragon's newlib; directory iteration uses dir_findfirst/findnext.

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

static bool sInitialized = false;

void SYS_Initialize()
{
    if (sInitialized) return;
    sInitialized = true;
    LogDebug("System_N64: initialised");
}

void SYS_Shutdown()
{
    sInitialized = false;
}

void SYS_Update() {}

std::string SYS_GetExecutablePath() { return "rom:/Polyphase.z64"; }
std::string SYS_GetPolyphasePath()  { return "rom:/"; }
std::string SYS_GetCurrentDirectoryPath() { return "rom:/"; }

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    if (relativePath.size() >= 4 && relativePath.compare(0, 4, "rom:") == 0) return relativePath;
    if (relativePath.size() >= 3 && relativePath.compare(0, 3, "sd:") == 0)  return relativePath;
    return SYS_GetPolyphasePath() + relativePath;
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}
void SYS_SetWorkingDirectory(const std::string& /*dirPath*/) {}

namespace
{
    // Engine paths are bare (e.g. "Assets/Foo.oct"). libdragon's newlib
    // routes by filesystem prefix: rom:/ → DragonFS, sd:/ → flashcart.
    inline std::string ToRomPath(const char* path)
    {
        if (path == nullptr || path[0] == '\0') return std::string();
        if (std::strncmp(path, "rom:",  4) == 0) return std::string(path);
        if (std::strncmp(path, "sd:",   3) == 0) return std::string(path);
        if (path[0] == '.' && path[1] == '/') path += 2;
        std::string out = "rom:/";
        out += path;
        return out;
    }
}

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    const std::string p = ToRomPath(path);
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void SYS_AcquireFileData(const char* path, bool /*isAsset*/, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    const std::string p = ToRomPath(path);
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f == nullptr)
    {
        LogWarning("SYS_AcquireFileData: fopen failed for '%s' (rom path: '%s')", path, p.c_str());
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
        // DragonFS over PI can short-read at EOF; cap to what arrived.
        actualSize = (uint32_t)read;
    }
    outSize = actualSize;
}

void SYS_ReleaseFileData(char* data)
{
    std::free(data);
}

bool SYS_CreateDirectory(const char* /*dirPath*/) { return false; }
void SYS_RemoveDirectory(const char* /*dirPath*/) {}

namespace
{
    struct N64DirIter
    {
        dir_t       mDir;
        std::string mPath;
        bool        mPrimed = false;
    };
}

void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    outDirEntry.mValid     = false;
    outDirEntry.mDirHandle = nullptr;
    outDirEntry.mFilename[0]      = '\0';
    outDirEntry.mDirectoryPath[0] = '\0';

    const std::string romPath = ToRomPath(dirPath.c_str());
    std::string queryPath = romPath;
    // dir_findfirst requires a trailing slash.
    if (queryPath.empty() || queryPath.back() != '/') queryPath += '/';

    N64DirIter* it = new N64DirIter();
    it->mPath = queryPath;
    const int rc = dir_findfirst(queryPath.c_str(), &it->mDir);
    if (rc != 0)
    {
        delete it;
        return;
    }
    it->mPrimed = true;
    outDirEntry.mDirHandle = it;

    std::strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mDirectoryPath[MAX_PATH_SIZE] = '\0';
    outDirEntry.mValid = true;

    std::strncpy(outDirEntry.mLastName, it->mDir.d_name, sizeof(outDirEntry.mLastName) - 1);
    outDirEntry.mLastName[sizeof(outDirEntry.mLastName) - 1] = '\0';
    std::strncpy(outDirEntry.mFilename, it->mDir.d_name, MAX_PATH_SIZE);
    outDirEntry.mFilename[MAX_PATH_SIZE] = '\0';
    outDirEntry.mDirectory = (it->mDir.d_type == DT_DIR);
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    N64DirIter* it = static_cast<N64DirIter*>(dirEntry.mDirHandle);
    if (!dirEntry.mValid || it == nullptr)
    {
        dirEntry.mValid = false;
        return;
    }

    // First call consumes the entry primed by dir_findfirst.
    if (it->mPrimed)
    {
        it->mPrimed = false;
        return;
    }

    const int rc = dir_findnext(it->mPath.c_str(), &it->mDir);
    if (rc != 0)
    {
        dirEntry.mValid = false;
        return;
    }

    std::strncpy(dirEntry.mLastName, it->mDir.d_name, sizeof(dirEntry.mLastName) - 1);
    dirEntry.mLastName[sizeof(dirEntry.mLastName) - 1] = '\0';
    std::strncpy(dirEntry.mFilename, it->mDir.d_name, MAX_PATH_SIZE);
    dirEntry.mFilename[MAX_PATH_SIZE] = '\0';
    dirEntry.mDirectory = (it->mDir.d_type == DT_DIR);
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    N64DirIter* it = static_cast<N64DirIter*>(dirEntry.mDirHandle);
    if (it != nullptr)
    {
        delete it;
        dirEntry.mDirHandle = nullptr;
    }
    dirEntry.mValid = false;
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

// Single-threaded stubs: returning a non-null handle silently skips the
// worker; engine subsystems that needed it degrade gracefully (e.g. async
// asset loads fall back to sync). Running the callable inline isn't safe
// because engine workers are long-lived loops, not one-shots.
ThreadObject* SYS_CreateThread(ThreadFuncFP /*func*/, void* /*arg*/)
{
    ThreadObject* out = new ThreadObject;
    *out = 1;
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
    ::wait_ms(milliseconds);
}

uint64_t SYS_GetTimeMicroseconds()
{
    // libdragon's TICKS_PER_SECOND = 46875000 (half the CPU clock).
    const uint64_t ticks = (uint64_t)::get_ticks();
    return ticks * 1000000ULL / 46875000ULL;
}

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// libdragon's <malloc.h> omits memalign though newlib provides it.
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
    // Stock: 4 MB, with Expansion Pak: 8 MB.
    return (GetEngineState() && GetEngineState()->mSystem.mExpansionPak)
        ? (float)(8u * 1024u * 1024u)
        : (float)(4u * 1024u * 1024u);
}
float SYS_GetTotalVRAM()   { return 0.0f; }
float SYS_GetTotalRAM1()   { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()   { return 0.0f; }

bool SYS_ReadSave(const char* /*saveName*/, Stream& /*outStream*/) { return false; }
bool SYS_WriteSave(const char* /*saveName*/, Stream& /*stream*/)   { return false; }
bool SYS_DoesSaveExist(const char* /*saveName*/)                   { return false; }
bool SYS_DeleteSave(const char* /*saveName*/)                      { return false; }
void SYS_UnmountMemoryCard() {}

void SYS_SetClipboardText(const std::string& /*str*/) {}
std::string SYS_GetClipboardText() { return ""; }

// debugf routes to ISViewer (Mupen/ares) and UART (usblog) — both fail-safe.
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
    return (GetEngineState() && GetEngineState()->mSystem.mExpansionPak) ? 1 : 0;
}

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
