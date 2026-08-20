#pragma once

#include <cstddef>
#include <limits>

namespace memalloc::detail {

constexpr std::size_t size_max = std::numeric_limits<std::size_t>::max();

constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

constexpr bool align_up_overflows(std::size_t value, std::size_t alignment) noexcept {
    return value > size_max - (alignment - 1);
}

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr bool product_overflows(std::size_t left, std::size_t right) noexcept {
    return left != 0 && right > size_max / left;
}

constexpr std::size_t max(std::size_t left, std::size_t right) noexcept {
    return left < right ? right : left;
}

}
