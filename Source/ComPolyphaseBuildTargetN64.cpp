// MSVC SDL deprecation suppression — every CRT call is bounds-checked.
#define _CRT_SECURE_NO_WARNINGS

/**
 * @file ComPolyphaseBuildTargetN64.cpp
 * @brief Nintendo 64 (libdragon) build-target addon for Polyphase Engine.
 *
 * Adds a "Nintendo 64 (libdragon)" entry to the editor's Build Profile
 * dropdown. Variant-2 layout — ships its own engine runtime under
 * Runtime/N64/ via `platformExtensionDir`, so the engine binary never
 * links against libdragon.
 *
 * Toolchain expectations on the host:
 *
 *   Default install prefix follows libdragon convention (`N64_INST`):
 *     - Windows native: C:\libdragon (mips64-elf-gcc.exe ships native)
 *     - Linux / WSL:    /opt/libdragon  (the canonical install dir)
 *
 *   Required artifacts inside the prefix:
 *     - $N64_INST/bin/mips64-elf-gcc[.exe]   — cross compiler
 *     - $N64_INST/mips64-elf/lib/libdragon.a — runtime library
 *     - $N64_INST/bin/n64tool[.exe]          — ROM packager
 *     - $N64_INST/include/n64.mk             — toolchain Makefile fragment
 *
 *   If only the toolchain is installed (no libdragon.a yet), Validate
 *   surfaces the canonical bootstrap command:
 *     git clone https://github.com/DragonMinded/libdragon
 *     cd libdragon && N64_INST=/opt/libdragon ./build.sh
 *
 *   Optional everywhere:
 *     - ares.exe / ares for emulator launch
 *       (N64_EMULATOR env var overrides the executable name).
 *
 * WSL routing on Windows is OFF by default — libdragon's toolchain ships
 * native Windows binaries, so running through WSL would cost ~2× build
 * time for no benefit. Users with a WSL-only install enable the toggle in
 * Target Options.
 *
 * Licensing isolation: the engine binary never links against libdragon.
 * Every libdragon / mips64-elf reference lives only in this addon and
 * the addon-shipped Runtime/N64/ tree (compiled into the .z64 by the
 * addon's Makefile_N64, not into the editor itself).
 *
 * Maintainer: Polyphase Engine team.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#include "imgui.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

#if EDITOR
namespace
{
    // ----- Helpers ----------------------------------------------------------

    std::string GetEnvOrEmpty(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty()) return false;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) { std::fclose(f); return true; }
        return false;
    }

    // ----- Per-profile option keys -----------------------------------------

    constexpr const char* kTitleKey         = "n64.title";          // ROM header title (max 20 chars after padding)
    constexpr const char* kRegionKey        = "n64.region";         // NTSC / PAL / MPAL
    constexpr const char* kSaveTypeKey      = "n64.saveType";       // none / eeprom4k / eeprom16k / sram256k / sram1m / flashram1m
    constexpr const char* kMakefileKey      = "n64.makefile";       // bare filename inside addon root, or absolute override
    constexpr const char* kJobsKey          = "n64.jobs";           // make -j parallelism (default 4)
    constexpr const char* kN64InstPathKey   = "n64.n64InstPath";    // absolute path to libdragon install prefix
    constexpr const char* kUseWslKey        = "n64.useWsl";         // Windows only — route commands through WSL
    constexpr const char* kExpansionPakKey  = "n64.expansionPak";   // require Expansion Pak (8 MB)

    constexpr const char* kTitleDefault     = "Polyphase Game";
    constexpr const char* kRegionDefault    = "NTSC";
    constexpr const char* kSaveTypeDefault  = "none";
    constexpr const char* kMakefileDefault  = "Makefile_N64";
    constexpr const char* kJobsDefault      = "4";

    // ----- Option I/O ------------------------------------------------------

    std::string ReadOption(const PolyphaseBuildContext* ctx, const char* key, const char* fallback)
    {
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return fallback ? fallback : "";
        char buf[512] = {0};
        if (ctx->GetProfileSetting(key, buf, sizeof(buf)) == 0 || buf[0] == '\0')
        {
            return fallback ? fallback : "";
        }
        return std::string(buf);
    }

    bool ReadBoolOption(const PolyphaseBuildContext* ctx, const char* key, bool fallback)
    {
        const std::string v = ReadOption(ctx, key, fallback ? "1" : "0");
        if (v.empty()) return fallback;
        return v == "1" || v == "true" || v == "TRUE" || v == "True";
    }

#if defined(_WIN32)
    // Translate a Windows absolute path into its default WSL2 mount-point:
    //   C:\Foo\Bar  ->  /mnt/c/Foo/Bar
    // Covers the standard /mnt/<drive>/ layout. Users with custom WSL mount
    // roots should switch the Use WSL toggle off and build natively instead.
    std::string WinToWslPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath; // already POSIX
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/mnt/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
            {
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            }
            return out;
        }
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }

    // Normalize backslashes to forward slashes — make's path expansion
    // chokes on backslashes regardless of host. Used for cmd-shell paths
    // when WSL is disabled.
    std::string WinNormalizePath(const std::string& winPath)
    {
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }
#endif

    // Quote a path for the chosen shell. When `wsl` is true we emit
    // /mnt/<drive>/... single-quoted; otherwise we emit the path as-is
    // with forward slashes inside double quotes (cmd-friendly).
    std::string ShellPath(const std::string& path, bool wsl)
    {
#if defined(_WIN32)
        if (wsl)
        {
            return std::string("'") + WinToWslPath(path) + "'";
        }
        return std::string("\"") + WinNormalizePath(path) + "\"";
#else
        (void)wsl;
        return std::string("'") + path + "'";
#endif
    }

    // Wrap a build-shell body for execution by std::system. Three modes:
    //   1. POSIX host:                    `bash -lc "<body>"`
    //   2. Windows + Use WSL off:         `cmd /c "<body>"` with N64_INST/bin pre-pended to PATH
    //   3. Windows + Use WSL on:          `wsl bash -lc "<body>"`
    // The body should already use shell-appropriate path quoting (ShellPath).
    std::string WrapShell(const PolyphaseBuildContext* /*ctx*/, const std::string& body, bool wsl, const std::string& n64Inst)
    {
#if defined(_WIN32)
        if (wsl)
        {
            return std::string("wsl bash -lc \"") + body + "\"";
        }
        // Native Windows: prepend the toolchain bin dir so make's recursive
        // mips64-elf-gcc / n64tool / chksum64 invocations are reachable.
        // Use double-quoted cmd /c so embedded paths/spaces survive.
        std::string pathSet;
        if (!n64Inst.empty())
        {
            pathSet = "set PATH=" + WinNormalizePath(n64Inst) + "/bin;%PATH%&&";
        }
        return std::string("cmd /c \"") + pathSet + body + "\"";
#else
        (void)wsl;
        (void)n64Inst;
        return std::string("bash -lc \"") + body + "\"";
#endif
    }

    // Resolve N64_INST for the build shell:
    //   1. Per-profile override (n64.n64InstPath) — UI-side override
    //   2. N64_INST env var (only if shape matches the chosen routing)
    //   3. Platform default — C:\libdragon (Win native) or /opt/libdragon (POSIX/WSL)
    //
    // The env-var step is shape-aware: when useWsl=true on Windows, a
    // Windows-style env value (`C:\libdragon`) is IGNORED in favor of the
    // POSIX default `/opt/libdragon`. Otherwise an env left over from the
    // libdragon-binaries Windows release silently overrides the user's
    // explicit "Use WSL" toggle and the build inside WSL would look for
    // libdragon at `C:/libdragon` (which doesn't exist as a POSIX path).
    // Conversely, a `/opt/libdragon`-style env on Windows-native is also
    // skipped — no mips64-elf-gcc.exe at that path.
    std::string ResolveN64Inst(const PolyphaseBuildContext* ctx, bool useWsl)
    {
        const std::string override_ = ReadOption(ctx, kN64InstPathKey, "");
        if (!override_.empty()) return override_;

        const std::string env = GetEnvOrEmpty("N64_INST");
        if (!env.empty())
        {
            const bool envIsWindowsShaped =
                (env.size() >= 2 && env[1] == ':') ||  // C:\... or C:/...
                env.find('\\') != std::string::npos;
            const bool envIsPosixShaped = !envIsWindowsShaped && !env.empty() && env[0] == '/';

#if defined(_WIN32)
            if (useWsl && envIsWindowsShaped)
            {
                // User explicitly opted in to WSL routing — the
                // libdragon-binaries Windows env var would defeat them.
                // Fall through to the POSIX default below.
            }
            else if (!useWsl && envIsPosixShaped)
            {
                // Symmetric: a POSIX env value on a native-Windows build
                // path can't resolve. Fall through to the native default.
            }
            else
            {
                return env;
            }
#else
            (void)envIsPosixShaped;
            return env;
#endif
        }

#if defined(_WIN32)
        return useWsl ? std::string("/opt/libdragon") : std::string("C:\\libdragon");
#else
        (void)useWsl;
        return std::string("/opt/libdragon");
#endif
    }

    // Shell-side prelude that detects libdragon when N64_INST isn't set or
    // points at the wrong place. Mirrors the PSP addon's PSPDEV auto-detect
    // ladder. Run only inside bash bodies (WSL on Windows, native on POSIX);
    // the cmd-shell path uses ResolveN64Inst directly because there's no
    // need to grep a wide candidate list when we already know the user's
    // expected install prefix.
    std::string N64InstAutoDetectPrelude()
    {
        // /opt/libdragon listed FIRST — that's the canonical install dir
        // documented at libdragon.dev and used by every CI image. Other
        // candidates are fallbacks for non-standard layouts.
        return
            "for d in /opt/libdragon /usr/local/libdragon \\\"\\$HOME/libdragon\\\" "
            "/mnt/c/libdragon /mnt/d/libdragon; do "
            "if [ -x \\\"\\$d/bin/n64tool\\\" ]; then "
            "export N64_INST=\\\"\\$d\\\"; break; fi; done && "
            "export PATH=\\\"\\$N64_INST/bin:\\$PATH\\\"";
    }

    // ----- Build-target callbacks ------------------------------------------

    int32_t N64_Validate(char* outReason, size_t cap)
    {
        // Validate is called outside a build context (no PolyphaseBuildContext
        // available here in API v1), so we can only inspect env + the
        // canonical install paths. Per-profile n64InstPath / useWsl overrides
        // are NOT visible — Validate has to be conservative. The follow-up
        // build will surface a richer error if the user picked a custom path
        // and it's broken.

        const std::string envInst = GetEnvOrEmpty("N64_INST");

#if defined(_WIN32)
        // Try native Windows first. The libdragon Windows release sets
        // N64_INST=C:\libdragon by convention.
        const std::string winInst = envInst.empty() ? std::string("C:\\libdragon") : envInst;

        const std::string gccWin       = winInst + "\\bin\\mips64-elf-gcc.exe";
        const std::string libdragonWin = winInst + "\\mips64-elf\\lib\\libdragon.a";
        const std::string n64toolWin   = winInst + "\\bin\\n64tool.exe";

        if (FileExists(gccWin) && FileExists(libdragonWin) && FileExists(n64toolWin))
        {
            return 1;
        }

        // If gcc is present but libdragon.a isn't, the user installed the
        // toolchain only — surface the canonical bootstrap command.
        if (FileExists(gccWin) && !FileExists(libdragonWin))
        {
            std::snprintf(outReason, cap,
                "libdragon toolchain found at '%s' but libdragon.a is missing. "
                "Build the runtime once:\n"
                "  git clone https://github.com/DragonMinded/libdragon\n"
                "  cd libdragon && set N64_INST=%s && ./build.sh\n"
                "Or enable 'Use WSL' in Target Options and install via /opt/libdragon.",
                winInst.c_str(), winInst.c_str());
            return 0;
        }

        // Try WSL as a fallback — many devs run pspdev+libdragon WSL-side.
        const int wslRc = std::system(
            "wsl bash -lc \"[ -x /opt/libdragon/bin/n64tool ] || "
            "[ -x \\\"\\$N64_INST/bin/n64tool\\\" ] || "
            "command -v n64tool >/dev/null 2>&1\"");
        if (wslRc == 0)
        {
            // Hint to enable the WSL toggle — Validate can't toggle it
            // itself but the message tells the user how to make it work.
            std::snprintf(outReason, cap,
                "libdragon found inside WSL but not natively. Enable "
                "'Use WSL' in Target Options to route the build through "
                "WSL, OR install libdragon Windows-side at C:\\libdragon.");
            return 0;
        }

        std::snprintf(outReason, cap,
            "libdragon not found. Install it via one of:\n"
            "  - Windows: download libdragon-binaries release, install to C:\\libdragon, set N64_INST.\n"
            "  - WSL/Linux: git clone https://github.com/DragonMinded/libdragon && cd libdragon && N64_INST=/opt/libdragon ./build.sh\n"
            "Set N64_INST env var, or enter the path in Target Options > 'N64_INST path'.");
        return 0;
#else
        const std::string posixInst = envInst.empty() ? std::string("/opt/libdragon") : envInst;

        const std::string gcc       = posixInst + "/bin/mips64-elf-gcc";
        const std::string libdragon = posixInst + "/mips64-elf/lib/libdragon.a";
        const std::string n64tool   = posixInst + "/bin/n64tool";

        if (FileExists(gcc) && FileExists(libdragon) && FileExists(n64tool))
        {
            return 1;
        }

        if (FileExists(gcc) && !FileExists(libdragon))
        {
            std::snprintf(outReason, cap,
                "libdragon toolchain found at '%s' but libdragon.a is missing. "
                "Build the runtime once:\n"
                "  git clone https://github.com/DragonMinded/libdragon\n"
                "  cd libdragon && N64_INST=%s ./build.sh",
                posixInst.c_str(), posixInst.c_str());
            return 0;
        }

        std::snprintf(outReason, cap,
            "libdragon not found at '%s'. Install via:\n"
            "  git clone https://github.com/DragonMinded/libdragon && cd libdragon && N64_INST=%s ./build.sh\n"
            "Or set N64_INST to your install prefix, or enter the path in Target Options.",
            posixInst.c_str(), posixInst.c_str());
        return 0;
#endif
    }

    // Test whether the libdragon runtime (libdragon.a + n64tool) is reachable
    // at a given Windows-side prefix. Used by GetCompileCommand to decide
    // whether to auto-fall-back to WSL routing on Windows hosts.
    bool LibdragonReadyAtWindowsPath(const std::string& winInst)
    {
        const std::string libdragonWin = winInst + "\\mips64-elf\\lib\\libdragon.a";
        const std::string n64toolWin   = winInst + "\\bin\\n64tool.exe";
        // Forward-slash variant for paths like C:/libdragon set by the env.
        const std::string libdragonAlt = winInst + "/mips64-elf/lib/libdragon.a";
        const std::string n64toolAlt   = winInst + "/bin/n64tool.exe";
        return (FileExists(libdragonWin) || FileExists(libdragonAlt))
            && (FileExists(n64toolWin)   || FileExists(n64toolAlt));
    }

    bool LibdragonReadyInsideWsl()
    {
#if defined(_WIN32)
        // Synchronous probe — runs `wsl bash -lc "[ -f X ] && [ -f Y ]"`.
        // Cost: ~150 ms first time WSL starts, cached after that.
        const int rc = std::system(
            "wsl bash -lc \"[ -f /opt/libdragon/mips64-elf/lib/libdragon.a ] && "
            "[ -x /opt/libdragon/bin/n64tool ]\" >nul 2>&1");
        return rc == 0;
#else
        return false;
#endif
    }

    int32_t N64_GetCompileCommand(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr) return 0;

        bool useWsl = ReadBoolOption(ctx, kUseWslKey, false);
        std::string n64Inst = ResolveN64Inst(ctx, useWsl);
        const char* sourceLabel =
            !ReadOption(ctx, kN64InstPathKey, "").empty() ? "profile override"
          : !GetEnvOrEmpty("N64_INST").empty()            ? "env var"
          :                                                 "platform default";

        // Auto-fallback: on Windows, if useWsl is off AND the resolved
        // Windows-side N64_INST doesn't have libdragon.a, AND WSL has
        // libdragon at /opt/libdragon, switch to WSL routing automatically.
        // The user almost certainly wanted the libdragon they actually
        // built — sticking with the broken Windows-side path would just
        // fail at make's guard with no clear next step.
#if defined(_WIN32)
        if (!useWsl
            && ReadOption(ctx, kN64InstPathKey, "").empty()   // no explicit override
            && !LibdragonReadyAtWindowsPath(n64Inst)
            && LibdragonReadyInsideWsl())
        {
            useWsl = true;
            n64Inst = "/opt/libdragon";
            sourceLabel = "auto-fallback (Windows-side libdragon missing; WSL /opt/libdragon found)";
        }
#endif

        // Build the diagnostic string. Sent THREE ways so it's impossible
        // to miss:
        //   1. Through ctx->WriteOutputLine — official addon API path.
        //   2. Through ctx->Log — engine log file (backup).
        //   3. Prepended to the command itself as an `echo` so it lands
        //      in build_log.txt (which only captures the shell process'
        //      stdout). Without #3 we get the same "diagnostic invisible
        //      in build_log" symptom we just hit.
        char diag[512];
        std::snprintf(diag, sizeof(diag),
            "N64 build target [DLL built %s %s]: useWsl=%s N64_INST=%s (source: %s)",
            __DATE__, __TIME__,
            useWsl ? "yes" : "no",
            n64Inst.c_str(),
            sourceLabel);
        if (ctx->WriteOutputLine != nullptr) ctx->WriteOutputLine(diag);
        if (ctx->Log != nullptr)             ctx->Log(POLYPHASE_BT_LOG_DEBUG, diag);

        const std::string makefileOpt = ReadOption(ctx, kMakefileKey, kMakefileDefault);
        const std::string jobsOpt     = ReadOption(ctx, kJobsKey,     kJobsDefault);
        const std::string title       = ReadOption(ctx, kTitleKey,    kTitleDefault);
        const std::string region      = ReadOption(ctx, kRegionKey,   kRegionDefault);
        const bool expansionPak       = ReadBoolOption(ctx, kExpansionPakKey, false);

        // Validate jobs option — must be a positive decimal int.
        int jobs = 0;
        for (char c : jobsOpt) { if (c < '0' || c > '9') { jobs = 0; break; } jobs = jobs * 10 + (c - '0'); }
        if (jobs < 1 || jobs > 64) jobs = 4;

        // Resolve makefile path. Bare filenames default to
        // <projectDir>/Packages/com.polyphase.build.target.n64/<file>.
        // Absolute overrides are taken as-is.
        const bool isAbsolute =
            !makefileOpt.empty() &&
            (makefileOpt[0] == '/' ||
             (makefileOpt.size() >= 2 && makefileOpt[1] == ':'));
        const std::string makefilePath = isAbsolute
            ? makefileOpt
            : (std::string(ctx->projectDir) +
               "/Packages/com.polyphase.build.target.n64/" + makefileOpt);

        // make-line variable assignments (take precedence over Makefile body
        // assignments, which is what we want).
        std::string makeN64Inst;
        if (!n64Inst.empty())
        {
            // For WSL routing the path needs Linux semantics; for native
            // Windows we pass the path as-is (make handles Windows-style
            // forward-slash paths).
            const std::string forShell = useWsl
                ? std::string("'") +
#if defined(_WIN32)
                  WinToWslPath(n64Inst)
#else
                  n64Inst
#endif
                  + "'"
                : ShellPath(n64Inst, useWsl);
            makeN64Inst = " N64_INST=" + forShell;
        }

        std::string makePolyphasePath;
        if (ctx->engineDir != nullptr && ctx->engineDir[0] != '\0')
        {
            makePolyphasePath = " POLYPHASE_PATH=" + ShellPath(ctx->engineDir, useWsl);
        }

        // Always pass PROJECT_ROOT explicitly so the Makefile doesn't have
        // to derive it from CURDIR.
        const std::string makeProjectRoot =
            " PROJECT_ROOT=" + ShellPath(ctx->projectDir, useWsl);

        char jobsArg[16];
        std::snprintf(jobsArg, sizeof(jobsArg), " -j%d", jobs);

        const std::string intermediateDir =
            std::string(ctx->projectDir) + "/Intermediate/N64";
        const std::string buildDir =
            std::string(ctx->projectDir) + "/Build/N64";

        // Body assembly is host-aware. Bash (POSIX host OR Windows-with-WSL)
        // gets the standard `mkdir -p ... && rm -f ... && make ...` chain.
        // Native Windows runs under cmd.exe which doesn't grok `mkdir -p`,
        // `rm`, `2>/dev/null`, or `; true` — use CMD-native equivalents:
        //   `if not exist "X" mkdir "X"` (CMD's mkdir creates parents
        //                                 implicitly when extensions are on,
        //                                 which they are since Win2k+)
        //   `del /q X >nul 2>&1`         (replaces `rm -f X 2>/dev/null`)
        //   `&&` works in CMD natively (since Win2k+).
        //
        // The two emitted bodies are otherwise structurally identical so
        // the make invocation that follows works regardless of host.
#if defined(_WIN32)
        const bool useBashShell = useWsl;
#else
        const bool useBashShell = true;
#endif

        // ROM-side metadata threaded through to the Makefile's n64tool
        // invocation. Title quoting depends on the chosen shell — bash
        // accepts single-quoted strings, cmd.exe accepts double-quoted
        // (and inner quotes survive `cmd /c "..."` per CMD's rule-#2
        // "strip first and last quote only" parsing). Title is capped to
        // 20 characters by n64tool itself; we just preserve spaces.
        auto quoteForBash = [](const std::string& s) {
            std::string out = "'";
            for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
            out += '\''; return out;
        };
        std::string makeRomMeta =
            " N64_ROM_TITLE=" + (useBashShell
                ? quoteForBash(title)
                : std::string("\"") + title + "\"") +
            " N64_ROM_REGION=" + region;
        if (expansionPak) makeRomMeta += " N64_EXPANSION_PAK=1";

        // Echo prefix that lands in build_log.txt. Naive `echo X && ...`
        // breaks in bash when X contains `(`, `)`, `<`, `>`, `&`, `|`, `;`
        // (any shell metacharacter). Quote per-shell:
        //   - bash: single-quote the whole diag. The diag string never
        //     contains a literal single quote so no escape needed.
        //   - cmd:  double-quote, escape any literal `"` to `\"` first.
        // The outer `bash -lc "..."` or `cmd /c "..."` wrap survives this
        // because their first-and-last-quote-stripping respects the inner
        // matched pair.
        std::string echoDiag;
        if (useBashShell)
        {
            // Replace any single-quote chars (none expected, but defensive)
            // using the bash-safe `'\''` close-then-reopen pattern.
            std::string safe;
            for (const char* p = diag; *p; ++p)
            {
                if (*p == '\'') safe += "'\\''";
                else            safe += *p;
            }
            echoDiag = std::string("echo '") + safe + "' && ";
        }
        else
        {
            std::string safe;
            for (const char* p = diag; *p; ++p)
            {
                if (*p == '"') safe += "\\\"";
                else           safe += *p;
            }
            echoDiag = std::string("echo \"") + safe + "\" && ";
        }

        std::string body;

        if (useBashShell)
        {
            std::string mkIntermediate =
                "mkdir -p " + ShellPath(intermediateDir, useWsl) + " && ";

            std::string cleanPrefix;
            if (ctx->forceRebuild)
            {
                cleanPrefix =
                    "(cd " + ShellPath(intermediateDir, useWsl) +
                    " && rm -f *.o *.d *.elf *.bin 2>/dev/null; true) && " +
                    "(rm -f " + ShellPath(buildDir, useWsl) +
                    "/*.z64 2>/dev/null; true) && ";
            }

            body =
                echoDiag +
                mkIntermediate +
                cleanPrefix +
                "make -C " + ShellPath(intermediateDir, useWsl) +
                " -f " + ShellPath(makefilePath, useWsl) +
                makeProjectRoot + makeN64Inst + makePolyphasePath +
                makeRomMeta + jobsArg;
        }
        else
        {
            // CMD-native body. ShellPath(..., false) emits forward-slash
            // double-quoted paths which CMD tolerates. mkdir is wrapped in
            // `if not exist` so a pre-existing dir doesn't error out.
            std::string mkIntermediate =
                "if not exist " + ShellPath(intermediateDir, false) +
                " mkdir " + ShellPath(intermediateDir, false) + " && ";

            std::string cleanPrefix;
            if (ctx->forceRebuild)
            {
                // `del` takes a path-with-pattern; chain multiple via `&`
                // so they all run even if some find nothing. `>nul 2>&1`
                // suppresses "could not find" noise.
                cleanPrefix =
                    "del /q " + ShellPath(intermediateDir + "/*.o",   false) + " >nul 2>&1 & " +
                    "del /q " + ShellPath(intermediateDir + "/*.d",   false) + " >nul 2>&1 & " +
                    "del /q " + ShellPath(intermediateDir + "/*.elf", false) + " >nul 2>&1 & " +
                    "del /q " + ShellPath(intermediateDir + "/*.bin", false) + " >nul 2>&1 & " +
                    "del /q " + ShellPath(buildDir + "/*.z64",        false) + " >nul 2>&1 & ";
            }

            body =
                echoDiag +
                mkIntermediate +
                cleanPrefix +
                "make -C " + ShellPath(intermediateDir, false) +
                " -f " + ShellPath(makefilePath, false) +
                makeProjectRoot + makeN64Inst + makePolyphasePath +
                makeRomMeta + jobsArg;
        }

        std::snprintf(outCmd, cap, "%s", WrapShell(ctx, body, useWsl, n64Inst).c_str());
        return 1;
    }

    int32_t N64_GetCompiledBinaryPath(const PolyphaseBuildContext* ctx, char* outPath, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr || ctx->projectName == nullptr) return 0;

        // Makefile_N64's stage: target produces a `.z64` (not an ELF) at
        // this path. The engine's post-build mtime check picks it up and
        // copies it into Packaged/<id>/. PostPackage then runs against the
        // copied ROM for any final adjustments (Config.ini patch).
        std::snprintf(outPath, cap, "%s/Build/N64/%s.z64",
                      ctx->projectDir, ctx->projectName);
        return 1;
    }

    // Force `WindowWidth=320` / `WindowHeight=240` in a packaged Config.ini.
    // N64 default video mode is 320x240 (NTSC) — the project's authored
    // desktop resolution (typically 1280x720) gets reloaded by the engine's
    // ReadEngineConfig at boot and overrides the OctPreInitialize defaults
    // unless this rewrite step runs.
    void ForceN64WindowSizeInConfig(const std::string& configPath)
    {
        std::ifstream in(configPath);
        if (!in.is_open()) return;

        std::ostringstream out;
        std::string line;
        bool sawW = false;
        bool sawH = false;
        while (std::getline(in, line))
        {
            if (line.rfind("WindowWidth=", 0) == 0)
            {
                out << "WindowWidth=320\n";
                sawW = true;
            }
            else if (line.rfind("WindowHeight=", 0) == 0)
            {
                out << "WindowHeight=240\n";
                sawH = true;
            }
            else
            {
                out << line << "\n";
            }
        }
        in.close();

        if (!sawW) out << "WindowWidth=320\n";
        if (!sawH) out << "WindowHeight=240\n";

        std::ofstream o(configPath, std::ios::trunc);
        if (o.is_open()) o << out.str();
    }

    int32_t N64_PostPackage(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        const std::string outDir       = ctx->packageOutputDir;
        const std::string projectName  = ctx->projectName;
        const std::string projectDir   = ctx->projectDir ? ctx->projectDir : "";

        // (1) Patch Config.ini to N64 native 320x240.
        ForceN64WindowSizeInConfig(outDir + "/Config.ini");
        ForceN64WindowSizeInConfig(outDir + "/" + projectName + "/Config.ini");
        if (ctx->Log != nullptr)
        {
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, "Patched WindowWidth/Height to 320/240 in packaged Config.ini");
        }

        // (2) Bundle cooked assets / scripts / config into a DragonFS image
        //     and re-pack the ROM with ELF + sym + DFS via n64tool.
        //
        // Makefile_N64's `stage` target produced a bare-ELF .z64 in
        // <projectDir>/Build/N64/<projectName>.z64 which the engine just
        // copied here. That ROM has no asset payload — the engine's
        // AssetManager finds nothing at runtime and `No default scene
        // found` is the symptom. To fix, build a .dfs from <outDir> (which
        // already contains the cooked project tree under <projectName>/)
        // and re-wrap ELF + sym + DFS into a new .z64 that replaces the
        // bare one.
        bool useWsl = ReadBoolOption(ctx, kUseWslKey, false);
        std::string n64Inst = ResolveN64Inst(ctx, useWsl);

        // Auto-fallback same as GetCompileCommand — if Windows-side N64_INST
        // doesn't have libdragon but WSL does, switch transparently.
#if defined(_WIN32)
        if (!useWsl
            && ReadOption(ctx, kN64InstPathKey, "").empty()
            && !LibdragonReadyAtWindowsPath(n64Inst)
            && LibdragonReadyInsideWsl())
        {
            useWsl = true;
            n64Inst = "/opt/libdragon";
        }
#endif

        const std::string intermediate = projectDir + "/Intermediate/N64";
        const std::string elfStripped  = intermediate + "/" + projectName + ".elf.stripped";
        const std::string elfSym       = intermediate + "/" + projectName + ".elf.sym";
        const std::string dfsOut       = intermediate + "/" + projectName + ".dfs";
        // mkdfs roots at packageOutputDir (NOT the project subdir) so files
        // inside the DFS are addressable at exactly the paths the engine
        // queries: "<projectName>/<projectName>.octp", "<projectName>/Assets/...",
        // and the root-level "Config.ini". Rooting at the project subdir
        // strips the <projectName>/ prefix and the engine can't find anything.
        const std::string dfsRoot      = outDir;
        const std::string z64Out       = outDir + "/" + projectName + ".z64";

        const std::string title  = ReadOption(ctx, kTitleKey,  kTitleDefault);
        const std::string romSize = "32M";  // matches Makefile_N64 default

        auto quoteForBash = [](const std::string& s) {
            std::string out = "'";
            for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
            out += '\''; return out;
        };

        // Build the shell body. Two phases: mkdfs, then n64tool. Chained
        // with `&&` so the n64tool step only runs if mkdfs succeeded.
        std::string body;
#if defined(_WIN32)
        const bool useBashShell = useWsl;
#else
        const bool useBashShell = true;
#endif
        const std::string titleArg = useBashShell
            ? quoteForBash(title)
            : std::string("\"") + title + "\"";

        if (useBashShell)
        {
            body =
                "echo '[N64 PostPackage] Removing bare-ELF .z64 from DFS root...' && " +
                std::string("rm -f ") + ShellPath(z64Out, useWsl) + " && " +
                "echo '[N64 PostPackage] Bundling DragonFS...' && " +
                ShellPath(n64Inst + "/bin/mkdfs", useWsl) + " " +
                ShellPath(dfsOut, useWsl) + " " + ShellPath(dfsRoot, useWsl) + " >/dev/null && " +
                "echo '[N64 PostPackage] Re-wrapping ROM with ELF + sym + DFS...' && " +
                ShellPath(n64Inst + "/bin/n64tool", useWsl) +
                " --title " + titleArg +
                " -l " + romSize +
                " --toc" +
                " --output " + ShellPath(z64Out, useWsl) +
                " --align 256 " + ShellPath(elfStripped, useWsl) +
                " --align 8 "   + ShellPath(elfSym,      useWsl) +
                " --align 16 "  + ShellPath(dfsOut,      useWsl);
        }
        else
        {
            // Windows-native CMD. Same logical steps.
            body =
                "echo [N64 PostPackage] Removing bare-ELF .z64 from DFS root... && " +
                std::string("del /q ") + ShellPath(z64Out, false) + " >nul 2>&1 & " +
                "echo [N64 PostPackage] Bundling DragonFS... && " +
                ShellPath(n64Inst + "/bin/mkdfs", false) + " " +
                ShellPath(dfsOut, false) + " " + ShellPath(dfsRoot, false) + " >nul && " +
                "echo [N64 PostPackage] Re-wrapping ROM with ELF + sym + DFS... && " +
                ShellPath(n64Inst + "/bin/n64tool", false) +
                " --title " + titleArg +
                " -l " + romSize +
                " --toc" +
                " --output " + ShellPath(z64Out, false) +
                " --align 256 " + ShellPath(elfStripped, false) +
                " --align 8 "   + ShellPath(elfSym,      false) +
                " --align 16 "  + ShellPath(dfsOut,      false);
        }

        const std::string cmd = WrapShell(ctx, body, useWsl, n64Inst);
        if (ctx->WriteOutputLine != nullptr) ctx->WriteOutputLine(cmd.c_str());
        const int rc = std::system(cmd.c_str());
        if (rc != 0)
        {
            if (ctx->Log != nullptr)
            {
                char msg[512];
                std::snprintf(msg, sizeof(msg),
                    "N64 PostPackage: mkdfs/n64tool failed (rc=%d). ROM left as bare-ELF; engine will run with no on-disk assets.",
                    rc);
                ctx->Log(POLYPHASE_BT_LOG_WARNING, msg);
            }
            // Non-fatal: the bare-ELF .z64 still boots, just without assets.
            // Returning 0 would mark the whole package as failed, which is
            // too aggressive when this only affects asset availability.
            return 1;
        }

        if (ctx->Log != nullptr)
        {
            char ok[512];
            std::snprintf(ok, sizeof(ok),
                "N64 package complete with DragonFS: %s/%s.z64",
                outDir.c_str(), ctx->projectName);
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
        }
        return 1;
    }

    int32_t N64_RunInEmulator(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        const std::string override_ = GetEnvOrEmpty("N64_EMULATOR");
#if defined(_WIN32)
        const std::string exe = override_.empty()
            ? std::string("ares.exe")
            : override_;
#else
        const std::string exe = override_.empty()
            ? std::string("ares")
            : override_;
#endif

        std::snprintf(outCmd, cap,
            "\"%s\" \"%s/%s.z64\"",
            exe.c_str(),
            ctx->packageOutputDir,
            ctx->projectName);
        return 1;
    }

    int32_t N64_RunOnDevice(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        // UNFLoader is the de-facto USB ROM loader for ED64 / EverDrive 3.0
        // / 64Drive. Users point N64_USB_LOADER at the binary; we hand it
        // the .z64 with the conventional `-r <rom>` flag.
        const std::string loader = GetEnvOrEmpty("N64_USB_LOADER");
        if (loader.empty())
        {
            std::snprintf(outCmd, cap,
                "echo \"N64_USB_LOADER not set. Install UNFLoader from "
                "https://github.com/buu342/N64-UNFLoader and set "
                "N64_USB_LOADER=<path-to-UNFLoader.exe>\" && exit 1");
            return 1;
        }

        std::snprintf(outCmd, cap,
            "\"%s\" -r \"%s/%s.z64\"",
            loader.c_str(),
            ctx->packageOutputDir,
            ctx->projectName);
        return 1;
    }

    void N64_DrawProfileOptions(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->SetProfileSetting == nullptr) return;

        // ----- ROM header -------------------------------------------------
        ImGui::SeparatorText("ROM Header");
        {
            std::string current = ReadOption(ctx, kTitleKey, kTitleDefault);
            char buf[32] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Title", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kTitleKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Embedded into the ROM header. Max 20 ASCII chars (n64tool truncates).");
        }
        {
            static const char* kRegions[] = { "NTSC", "PAL", "MPAL" };
            std::string current = ReadOption(ctx, kRegionKey, kRegionDefault);
            int idx = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kRegions); ++i)
                if (current == kRegions[i]) { idx = i; break; }
            if (ImGui::Combo("Region", &idx, kRegions, IM_ARRAYSIZE(kRegions)))
            {
                ctx->SetProfileSetting(kRegionKey, kRegions[idx]);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ROM region byte. NTSC = 60 Hz (US/JP); PAL = 50 Hz (EU); MPAL = 60 Hz Brazil. Most homebrew ships NTSC.");
        }

        // ----- Save type --------------------------------------------------
        ImGui::SeparatorText("Cartridge Save");
        {
            static const char* kSaves[] = { "none", "eeprom4k", "eeprom16k", "sram256k", "sram1m", "flashram1m" };
            std::string current = ReadOption(ctx, kSaveTypeKey, kSaveTypeDefault);
            int idx = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kSaves); ++i)
                if (current == kSaves[i]) { idx = i; break; }
            if (ImGui::Combo("Save Type", &idx, kSaves, IM_ARRAYSIZE(kSaves)))
            {
                ctx->SetProfileSetting(kSaveTypeKey, kSaves[idx]);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cartridge save chip. EEPROM is for small save data (high scores); SRAM/FlashRAM for full saves. 'none' = no save support.");
        }

        // ----- Build ------------------------------------------------------
        ImGui::SeparatorText("Build");
        {
            std::string current = ReadOption(ctx, kMakefileKey, kMakefileDefault);
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("Makefile", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kMakefileKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Makefile that drives libdragon's n64.mk. Bare filename resolves inside the addon (default: Makefile_N64). Absolute paths point at a fork.");
        }
        {
            std::string current = ReadOption(ctx, kJobsKey, kJobsDefault);
            int jobs = std::atoi(current.c_str());
            if (jobs < 1 || jobs > 64) jobs = 4;
            if (ImGui::SliderInt("Parallel Jobs", &jobs, 1, 32))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", jobs);
                ctx->SetProfileSetting(kJobsKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("`make -j<N>` parallelism. Each engine compile job peaks at ~1 GB RAM.\nRule of thumb: jobs = min(CPU cores, host RAM GB / 2).");
        }
        {
            bool expansionPak = ReadBoolOption(ctx, kExpansionPakKey, false);
            if (ImGui::Checkbox("Require Expansion Pak (8 MB)", &expansionPak))
            {
                ctx->SetProfileSetting(kExpansionPakKey, expansionPak ? "1" : "0");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Defines N64_EXPANSION_PAK=1 in the build. ROM will refuse to boot without the 8 MB Expansion Pak. Recommended for Polyphase (4 MB stock is very tight).");
        }

        // ----- Toolchain --------------------------------------------------
        ImGui::SeparatorText("Toolchain");
        {
            std::string current = ReadOption(ctx, kN64InstPathKey, "");
            char buf[256] = {0};
            std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
            if (ImGui::InputText("N64_INST path", buf, sizeof(buf)))
            {
                ctx->SetProfileSetting(kN64InstPathKey, buf);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Absolute path to your libdragon install prefix.\n"
                    "  - Windows native: e.g. C:\\libdragon\n"
                    "  - Linux/WSL: e.g. /opt/libdragon (default)\n"
                    "Leave empty to use the N64_INST env var or the platform default.");
            }
        }
#if defined(_WIN32)
        {
            bool useWsl = ReadBoolOption(ctx, kUseWslKey, false);
            if (ImGui::Checkbox("Use WSL (Windows only)", &useWsl))
            {
                ctx->SetProfileSetting(kUseWslKey, useWsl ? "1" : "0");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "When ON, build commands run through `wsl bash -lc` with /mnt/c/... path translation.\n"
                    "Default OFF — libdragon ships native Windows binaries which build ~2x faster.\n"
                    "Enable only if your libdragon install lives inside WSL (e.g. /opt/libdragon).");
        }
#endif

        ImGui::Spacing();
        ImGui::TextDisabled("Requires libdragon — N64_INST/bin/mips64-elf-gcc, n64tool, libdragon.a.");
        ImGui::TextDisabled("Install: github.com/DragonMinded/libdragon -> N64_INST=/opt/libdragon ./build.sh");
        ImGui::TextDisabled("Default emulator: ares (override with N64_EMULATOR env var).");
        ImGui::TextDisabled("Set N64_USB_LOADER=<UNFLoader path> to enable 'Run on Device' via flashcart.");

        // ----- Resolved-state panel ---------------------------------------
        // Persistence is automatic — every ImGui control above commits its
        // value via `ctx->SetProfileSetting` the moment the user changes it,
        // and the engine's BuildProfile auto-writes to disk on profile-save.
        // No explicit Save button is needed. What WAS missing was a way to
        // SEE what the addon resolves at any moment, so you can confirm
        // your changes took effect without having to start a build. The
        // panel below renders the same logic GetCompileCommand uses, plus
        // a libdragon-reachability check that you can refresh on demand.
        ImGui::Spacing();
        ImGui::SeparatorText("Resolved State (live)");

        const bool resolvedUseWsl = ReadBoolOption(ctx, kUseWslKey, false);
        const std::string overrideVal = ReadOption(ctx, kN64InstPathKey, "");
        const std::string envVal      = GetEnvOrEmpty("N64_INST");

        // Walk the same fallback ladder the build does so the panel matches
        // what GetCompileCommand will actually pick.
        const_cast<PolyphaseBuildContext*>(ctx);
        std::string resolvedInst;
        const char* resolvedSource = "platform default";
        if (!overrideVal.empty())     { resolvedInst = overrideVal; resolvedSource = "profile override"; }
        else if (!envVal.empty())
        {
            const bool envWin = (envVal.size() >= 2 && envVal[1] == ':') || envVal.find('\\') != std::string::npos;
            const bool envPosix = !envWin && !envVal.empty() && envVal[0] == '/';
#if defined(_WIN32)
            if (resolvedUseWsl && envWin) { resolvedInst = "/opt/libdragon"; resolvedSource = "platform default (env C:\\ ignored under WSL)"; }
            else if (!resolvedUseWsl && envPosix) { resolvedInst = "C:\\libdragon"; resolvedSource = "platform default (env POSIX ignored on native)"; }
            else { resolvedInst = envVal; resolvedSource = "env var"; }
#else
            (void)envWin; (void)envPosix;
            resolvedInst = envVal; resolvedSource = "env var";
#endif
        }
        else
        {
#if defined(_WIN32)
            resolvedInst = resolvedUseWsl ? "/opt/libdragon" : "C:\\libdragon";
#else
            resolvedInst = "/opt/libdragon";
#endif
        }

        // Cached reachability check — re-runs only when the user presses
        // the Re-check button. Avoids the ~150ms `wsl` shell-out cost on
        // every UI frame.
        static bool sChecked       = false;
        static bool sLibdragonWin  = false;
        static bool sLibdragonWsl  = false;
        static std::string sLastCheckedInst;

        if (!sChecked || sLastCheckedInst != resolvedInst)
        {
            sLibdragonWin = LibdragonReadyAtWindowsPath(resolvedInst);
#if defined(_WIN32)
            sLibdragonWsl = LibdragonReadyInsideWsl();
#endif
            sLastCheckedInst = resolvedInst;
            sChecked = true;
        }

        ImGui::Text("Use WSL:        %s", resolvedUseWsl ? "yes" : "no");
        ImGui::Text("N64_INST:       %s", resolvedInst.c_str());
        ImGui::Text("Source:         %s", resolvedSource);
        ImGui::Text("libdragon @win: %s", sLibdragonWin ? "FOUND" : "missing");
#if defined(_WIN32)
        ImGui::Text("libdragon @wsl: %s", sLibdragonWsl ? "FOUND (/opt/libdragon)" : "missing");

        // Auto-fallback preview — show whether GetCompileCommand will
        // override the user's selection at build time.
        const bool wouldFallback =
            !resolvedUseWsl && overrideVal.empty()
            && !sLibdragonWin && sLibdragonWsl;
        if (wouldFallback)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                "Auto-fallback: build will switch to WSL routing "
                "(useWsl=yes, N64_INST=/opt/libdragon).");
        }
#endif

        if (ImGui::Button("Re-check libdragon"))
        {
            sChecked = false; // force the lazy probe above to refire next frame
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(re-runs the file/WSL probes)");
    }

    // Canonical descriptor. Strings are deep-copied by the registry; this
    // static instance just needs to outlive the RegisterBuildTarget call.
    static PolyphaseBuildTargetDesc gN64Target{};
}
#endif // EDITOR

// ----- Plugin lifecycle -----------------------------------------------------

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    if (api) api->LogDebug("com.polyphase.build.target.n64 loaded.");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI) sEngineAPI->LogDebug("com.polyphase.build.target.n64 unloaded.");
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* /*nodeFactory*/) {}
static void RegisterScriptFuncs(lua_State* L) { (void)L; }

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr) return;

    if (hooks->RegisterBuildTarget == nullptr)
    {
        if (sEngineAPI)
        {
            sEngineAPI->LogWarning("com.polyphase.build.target.n64: this engine "
                                   "build predates the build-target API (need plugin "
                                   "apiVersion >= 4). Target not registered.");
        }
        return;
    }

    gN64Target = {};
    gN64Target.apiVersion            = POLYPHASE_BUILD_TARGET_API_VERSION;
    gN64Target.targetId              = "homebrew.n64";
    gN64Target.displayName           = "Nintendo 64 (libdragon)";
    gN64Target.iconText              = "";
    gN64Target.category              = "Retro Consoles";
    gN64Target.basePlatform          = 1; /* Platform::Linux — MIPS/newlib-ELF cook compat */
    gN64Target.binaryExtension       = ".z64";
    gN64Target.requiresDocker        = 0;
    gN64Target.supportsRunOnDevice   = 1;
    gN64Target.supportsEmulator      = 1;
    gN64Target.Validate              = &N64_Validate;
    gN64Target.PreCook               = nullptr;
    gN64Target.CookAsset             = nullptr; // Linux cook is fine for Phase 1
    gN64Target.GetCompileCommand     = &N64_GetCompileCommand;
    gN64Target.GetCompiledBinaryPath = &N64_GetCompiledBinaryPath;
    gN64Target.PostPackage           = &N64_PostPackage;
    gN64Target.RunOnDevice           = &N64_RunOnDevice;
    gN64Target.RunInEmulator         = &N64_RunInEmulator;
    gN64Target.DrawProfileOptions    = &N64_DrawProfileOptions;
    gN64Target.SerializeProfileOptions   = nullptr;
    gN64Target.DeserializeProfileOptions = nullptr;

    // Variant 2: addon-shipped Runtime/N64/ provides the platform extension
    // headers (SystemTypes_Platform.h, InputTypes_Platform.h, etc.) and the
    // System / Input / Audio / Network / Graphics backends compiled into
    // the .z64 by Makefile_N64. ActionManager writes the
    // Generated/PolyphasePlatform_*.h bridge files that #include the
    // addon's headers via absolute path when this target is selected.
    gN64Target.platformExtensionDir = "Runtime/N64";

    hooks->RegisterBuildTarget(hookId, &gN64Target);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    if (desc == nullptr) return 1;
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.build.target.n64";
    desc->pluginVersion       = "1.0.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = nullptr;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
