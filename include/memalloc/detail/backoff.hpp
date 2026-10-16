#pragma once

#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#    include <intrin.h>
#endif

namespace memalloc::detail {

inline void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
    __yield();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
    __asm__ __volatile__("yield");
#endif
}

class backoff {
public:
    void pause() noexcept {
        for (std::uint32_t spin = 0; spin < spins_; ++spin) {
            cpu_relax();
        }
        if (spins_ < limit) {
            spins_ *= 2;
        }
    }

private:
    static constexpr std::uint32_t limit = 64;
    std::uint32_t spins_ = 1;
};

}
