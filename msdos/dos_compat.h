/* dos_compat.h: forced into every felixterminal file in the MS-DOS build
   (-include). DOS runs one thread, and DJGPP's libstdc++ is built without
   threads, so it has std::lock_guard and std::unique_lock but no mutex
   types to put in them: these are mutexes that never wait. */
#pragma once
#ifdef __cplusplus
#include <mutex>
#ifndef _GLIBCXX_HAS_GTHREADS          /* (a libstdc++ with threads has its own) */
namespace std {
struct mutex {
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }
};
struct recursive_mutex : mutex {};
}
#endif
#endif
