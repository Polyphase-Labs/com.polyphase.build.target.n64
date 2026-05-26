// Stubs for std:: threading types libdragon's libstdc++ omits (no
// _GLIBCXX_HAS_GTHREADS on this multilib). Force-included via Makefile -include
// so they land before libdragon's empty <mutex> / <condition_variable>.
// N64 is single-threaded; lock/wait/notify are intentionally no-ops.

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
