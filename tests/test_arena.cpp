#include "memalloc/arena_resource.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <stdexcept>
#include <vector>

#include "check.hpp"

namespace {

bool is_aligned(const void* p, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}

void allocations_are_contiguous_and_aligned() {
    memalloc::arena_resource arena(1024, 16);
    void* a = arena.allocate(1, 1);
    void* b = arena.allocate(8, 8);
    void* c = arena.allocate(16, 16);
    CHECK(is_aligned(b, 8));
    CHECK(is_aligned(c, 16));
    CHECK(static_cast<std::byte*>(a) < static_cast<std::byte*>(b));
    CHECK(static_cast<std::byte*>(b) < static_cast<std::byte*>(c));
    CHECK(arena.used() == 32);
    CHECK(arena.remaining() == 1024 - 32);
}

void the_default_alignment_serves_every_fundamental_type() {
    static_assert(memalloc::arena_resource::default_alignment >= 16,
                  "the default alignment must cover 16 byte SIMD types on every platform");
    static_assert(memalloc::arena_resource::default_alignment >= alignof(std::max_align_t),
                  "the default alignment must cover the widest fundamental alignment");
    memalloc::arena_resource arena(256);
    CHECK(is_aligned(arena.allocate(8, 16), 16));
    CHECK(is_aligned(arena.allocate(8, alignof(std::max_align_t)), alignof(std::max_align_t)));
}

void deallocate_is_a_no_op() {
    memalloc::arena_resource arena(64);
    void* a = arena.allocate(16, 8);
    arena.deallocate(a, 16, 8);
    void* b = arena.allocate(16, 8);
    CHECK(a != b);
    CHECK(arena.used() == 32);
}

void reset_reuses_the_buffer() {
    memalloc::arena_resource arena(64);
    void* a = arena.allocate(16, 8);
    arena.reset();
    CHECK(arena.used() == 0);
    void* b = arena.allocate(16, 8);
    CHECK(a == b);
    CHECK(arena.high_water_mark() == 16);
}

void exhaustion_throws() {
    memalloc::arena_resource arena(32);
    CHECK(arena.owns(arena.allocate(32, 1)));
    CHECK_THROWS_BAD_ALLOC(arena.allocate(1, 1));
}

void over_alignment_throws() {
    memalloc::arena_resource arena(256, 8);
    CHECK_THROWS_BAD_ALLOC(arena.allocate(8, 64));
}

void padding_never_walks_past_the_end() {
    memalloc::arena_resource arena(24, 8);
    CHECK(arena.owns(arena.allocate(17, 1)));
    CHECK_THROWS_BAD_ALLOC(arena.allocate(1, 8));
}

void constructor_rejects_impossible_arguments() {
    bool threw = false;
    try {
        memalloc::arena_resource arena(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        memalloc::arena_resource arena(64, 24);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void a_failed_allocation_leaves_the_arena_usable() {
    memalloc::arena_resource arena(64, 8);
    void* first = arena.allocate(32, 8);
    CHECK_THROWS_BAD_ALLOC(arena.allocate(64, 8));
    CHECK(arena.used() == 32);
    void* second = arena.allocate(32, 8);
    CHECK(second == static_cast<std::byte*>(first) + 32);
    CHECK(arena.remaining() == 0);
}

void owns_only_its_own_buffer() {
    memalloc::arena_resource arena(64);
    void* inside = arena.allocate(8, 8);
    int outside = 0;
    CHECK(arena.owns(inside));
    CHECK(!arena.owns(&outside));
}

void over_aligned_types_work_when_the_arena_is_over_aligned() {
    memalloc::arena_resource arena(256, 64);
    void* first = arena.allocate(8, 64);
    void* second = arena.allocate(8, 64);
    CHECK(is_aligned(first, 64));
    CHECK(is_aligned(second, 64));
    CHECK(static_cast<std::byte*>(second) - static_cast<std::byte*>(first) == 64);
}

void works_as_a_container_resource() {
    memalloc::arena_resource arena(64 * 1024);
    std::pmr::vector<int> values(&arena);
    for (int i = 0; i < 1000; ++i) {
        values.push_back(i);
    }
    CHECK(values.front() == 0);
    CHECK(values.back() == 999);
    CHECK(arena.used() > 0);
}

}

int main() {
    return test::run("test_arena", [] {
        allocations_are_contiguous_and_aligned();
        the_default_alignment_serves_every_fundamental_type();
        deallocate_is_a_no_op();
        reset_reuses_the_buffer();
        exhaustion_throws();
        over_alignment_throws();
        padding_never_walks_past_the_end();
        constructor_rejects_impossible_arguments();
        a_failed_allocation_leaves_the_arena_usable();
        owns_only_its_own_buffer();
        over_aligned_types_work_when_the_arena_is_over_aligned();
        works_as_a_container_resource();
    });
}
