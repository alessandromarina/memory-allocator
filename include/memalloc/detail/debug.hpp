#pragma once

#include <cstdio>
#include <cstdlib>

#if !defined(MEMALLOC_DEBUG_CHECKS)
#    if defined(NDEBUG)
#        define MEMALLOC_DEBUG_CHECKS 0
#    else
#        define MEMALLOC_DEBUG_CHECKS 1
#    endif
#endif

#if !defined(MEMALLOC_DEBUG_SLOW_CHECKS)
#    define MEMALLOC_DEBUG_SLOW_CHECKS 0
#endif

namespace memalloc {

using violation_handler = void (*)(const char*);

namespace detail {

inline void abort_on_violation(const char* message) {
    std::fprintf(stderr, "memalloc contract violation: %s\n", message);
    std::abort();
}

inline violation_handler& installed_violation_handler() {
    static violation_handler handler = &abort_on_violation;
    return handler;
}

[[noreturn]] inline void report_violation(const char* message) {
    installed_violation_handler()(message);
    std::fprintf(stderr, "memalloc violation handler returned: %s\n", message);
    std::abort();
}

}

inline violation_handler set_violation_handler(violation_handler handler) {
    violation_handler& slot = detail::installed_violation_handler();
    const violation_handler previous = slot;
    slot = handler != nullptr ? handler : &detail::abort_on_violation;
    return previous;
}

}

#define MEMALLOC_CHECK(condition, message)               \
    do {                                                 \
        if (!(condition)) {                              \
            memalloc::detail::report_violation(message); \
        }                                                \
    } while (false)

#if MEMALLOC_DEBUG_CHECKS
#    define MEMALLOC_DEBUG_CHECK(condition, message) MEMALLOC_CHECK(condition, message)
#else
#    define MEMALLOC_DEBUG_CHECK(condition, message) \
        do {                                         \
        } while (false)
#endif

#if MEMALLOC_DEBUG_SLOW_CHECKS
#    define MEMALLOC_SLOW_CHECK(condition, message) MEMALLOC_CHECK(condition, message)
#else
#    define MEMALLOC_SLOW_CHECK(condition, message) \
        do {                                        \
        } while (false)
#endif
