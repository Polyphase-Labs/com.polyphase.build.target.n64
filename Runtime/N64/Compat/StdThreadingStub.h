/**
 * @file StdThreadingStub.h
 * @brief No-op stubs for the std:: threading types libdragon's libstdc++
 *        omits on N64.
 *
 * Iterative discovery: libdragon's libstdc++ 14.2.0 for mips64-elf ships
 * MORE of the threading API than I initially assumed. Confirmed by reading
 * actual build errors:
 *
 *   PRESENT (no stub needed):
 *     - std::lock_guard<T>      (template, no OS deps)
 *     - std::unique_lock<T>     (template, no OS deps)
 *     - std::memory_order       (enum, no OS deps)
 *     - std::atomic<T>          (compiler intrinsics for MIPS LL/SC)
 *     - std::thread             (declared; constructor links against
 *                                pthread_create at link time, will fail
 *                                if engine code actually creates one —
 *                                if that happens, provide a dummy
 *                                pthread_create in System_N64.cpp)
 *     - std::this_thread::yield (declared in <thread>)
 *
 *   MISSING (need stub):
 *     - std::mutex
 *     - std::recursive_mutex
 *
 * std::condition_variable status is unknown — leave out of the stub
 * until/unless a build error shows it missing.
 *
 * Force-included via the Makefile's `-include` so the stub lands BEFORE
 * libdragon's `<mutex>` (which is empty on this config). The two classes
 * are no-ops — N64 is single-threaded so synchronization is unnecessary.
 *
 * Polluting `namespace std` is UB per the standard but is the canonical
 * pattern for console ports with reduced libstdc++ toolchains. Guarded
 * with `#if defined(PLATFORM_N64)` so this header is inert elsewhere.
 */

#pragma once

#if defined(PLATFORM_N64)

#include <stdint.h>

#define POLYPHASE_N64_STD_THREADING_STUB 1

namespace std
{
    class mutex
    {
    public:
        mutex() = default;
        ~mutex() = default;
        mutex(const mutex&) = delete;
        mutex& operator=(const mutex&) = delete;

        void lock()     {}
        void unlock()   {}
        bool try_lock() { return true; }
    };

    class recursive_mutex
    {
    public:
        recursive_mutex() = default;
        ~recursive_mutex() = default;
        recursive_mutex(const recursive_mutex&) = delete;
        recursive_mutex& operator=(const recursive_mutex&) = delete;

        void lock()     {}
        void unlock()   {}
        bool try_lock() { return true; }
    };

    // ----- condition_variable --------------------------------------------
    // libdragon's libstdc++ ships <condition_variable> as an empty header
    // (guarded by _GLIBCXX_HAS_GTHREADS). Stub the class with no-op
    // wait/notify — N64 is single-threaded so nothing ever needs to be
    // signaled; the worker threads that would call wait() are guarded out
    // separately (see HttpClient.cpp's #if !PLATFORM_N64).
    enum class cv_status { no_timeout, timeout };

    class condition_variable
    {
    public:
        condition_variable() = default;
        ~condition_variable() = default;
        condition_variable(const condition_variable&) = delete;
        condition_variable& operator=(const condition_variable&) = delete;

        void notify_one() {}
        void notify_all() {}

        template<typename Lock>
        void wait(Lock&) {}
        template<typename Lock, typename Pred>
        void wait(Lock&, Pred) {}
        template<typename Lock, typename Rep, typename Period>
        cv_status wait_for(Lock&, ...) { return cv_status::no_timeout; }
        template<typename Lock, typename Clock, typename Dur>
        cv_status wait_until(Lock&, ...) { return cv_status::no_timeout; }
    };
}

#endif // PLATFORM_N64
