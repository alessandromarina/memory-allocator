#pragma once

#include <cstdio>
#include <exception>
#include <new>

namespace test {

inline int failures = 0;

template <class Body>
int run(const char* name, Body body) {
    try {
        body();
    } catch (const std::exception& error) {
        std::printf("%s: unhandled exception: %s\n", name, error.what());
        return 2;
    } catch (...) {
        std::printf("%s: unhandled exception of an unknown type\n", name);
        return 2;
    }
    if (failures == 0) {
        std::printf("%s: all checks passed\n", name);
    }
    return failures == 0 ? 0 : 1;
}

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
