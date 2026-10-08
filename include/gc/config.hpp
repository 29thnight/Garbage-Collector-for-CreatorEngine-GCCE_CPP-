#pragma once

// Export macro for the single GC runtime. Every DLL and translation unit must
// share one runtime instance, so the non-template state lives in one library.
#if defined(GCCE_SHARED)
#    if defined(_WIN32)
#        if defined(GCCE_BUILDING)
#            define GC_API __declspec(dllexport)
#        else
#            define GC_API __declspec(dllimport)
#        endif
#    else
#        define GC_API __attribute__((visibility("default")))
#    endif
#else
#    define GC_API
#endif

// Owner-thread checks for root registration, reference stores, allocation and
// collection. Enabled by default in debug builds.
#if !defined(GC_THREAD_CHECKS)
#    if defined(NDEBUG)
#        define GC_THREAD_CHECKS 0
#    else
#        define GC_THREAD_CHECKS 1
#    endif
#endif
