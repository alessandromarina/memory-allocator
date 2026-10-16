#pragma once

#include <new>
#include <stdexcept>

#include "memalloc/detail/debug.hpp"

#if !defined(MEMALLOC_HAS_EXCEPTIONS)
#    if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#        define MEMALLOC_HAS_EXCEPTIONS 1
#    else
#        define MEMALLOC_HAS_EXCEPTIONS 0
#    endif
#endif

namespace memalloc::detail {

[[noreturn]] inline void throw_bad_alloc(const char* message_without_exceptions) {
#if MEMALLOC_HAS_EXCEPTIONS
    (void)message_without_exceptions;
    throw std::bad_alloc();
#else
    report_violation(message_without_exceptions);
#endif
}

[[noreturn]] inline void throw_invalid_argument(const char* message) {
#if MEMALLOC_HAS_EXCEPTIONS
    throw std::invalid_argument(message);
#else
    report_violation(message);
#endif
}

[[noreturn]] inline void throw_logic_error(const char* message) {
#if MEMALLOC_HAS_EXCEPTIONS
    throw std::logic_error(message);
#else
    report_violation(message);
#endif
}

}
