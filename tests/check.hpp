#pragma once

#include <cstdio>
#include <new>

namespace test {

inline int failures = 0;

}

#define CHECK(expr)                                                             \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++test::failures;                                                   \
        }                                                                       \
    } while (false)

#define CHECK_THROWS_BAD_ALLOC(expr)                                                       \
    do {                                                                                   \
        bool thrown = false;                                                               \
        try {                                                                              \
            (void)(expr);                                                                  \
        } catch (const std::bad_alloc&) {                                                  \
            thrown = true;                                                                 \
        }                                                                                  \
        if (!thrown) {                                                                     \
            std::printf("%s:%d expected std::bad_alloc: %s\n", __FILE__, __LINE__, #expr); \
            ++test::failures;                                                              \
        }                                                                                  \
    } while (false)
