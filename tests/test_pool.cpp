#include "memalloc/pool_resource.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#include "check.hpp"

namespace {

class counting_resource : public std::pmr::memory_resource {
public:
    std::size_t live_blocks = 0;
    std::size_t live_bytes = 0;

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++live_blocks;
        live_bytes += bytes;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        --live_blocks;
        live_bytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct particle {
    float position[3];
    float velocity[3];
    std::uint32_t id;
};

bool is_aligned(const void* p, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}

void blocks_are_sized_and_aligned() {
    memalloc::pool_resource pool(1, 1, 4);
    CHECK(pool.block_size() == sizeof(void*));
    void* block = pool.allocate(1, 1);
    CHECK(is_aligned(block, alignof(void*)));
}

void freed_blocks_come_back_first() {
    auto pool = memalloc::pool_resource::for_type<particle>(8);
    void* a = pool.allocate(sizeof(particle), alignof(particle));
    void* b = pool.allocate(sizeof(particle), alignof(particle));
    pool.deallocate(b, sizeof(particle), alignof(particle));
    pool.deallocate(a, sizeof(particle), alignof(particle));
    CHECK(pool.blocks_in_use() == 0);
    CHECK(pool.allocate(sizeof(particle), alignof(particle)) == a);
    CHECK(pool.allocate(sizeof(particle), alignof(particle)) == b);
}

void chunks_grow_on_demand() {
    memalloc::pool_resource pool(32, 8, 4);
    CHECK(pool.chunk_count() == 0);
    std::vector<void*> blocks;
    for (int i = 0; i < 9; ++i) {
        blocks.push_back(pool.allocate(32, 8));
    }
    CHECK(pool.chunk_count() == 3);
    CHECK(pool.blocks_in_use() == 9);
    CHECK(pool.blocks_reserved() == 12);
    for (void* block : blocks) {
        pool.deallocate(block, 32, 8);
    }
    CHECK(pool.blocks_in_use() == 0);
    CHECK(pool.chunk_count() == 3);
}

void oversized_or_overaligned_requests_throw() {
    memalloc::pool_resource pool(32, 8, 4);
    CHECK_THROWS_BAD_ALLOC(pool.allocate(33, 8));
    CHECK_THROWS_BAD_ALLOC(pool.allocate(32, 16));
}

void blocks_never_overlap_under_churn() {
    constexpr std::size_t count = 4096;
    auto pool = memalloc::pool_resource::for_type<std::uint64_t>(64);
    std::vector<std::uint64_t*> blocks(count);
    for (std::size_t i = 0; i < count; ++i) {
        blocks[i] = static_cast<std::uint64_t*>(pool.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
        *blocks[i] = i;
    }
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 generator(20260820);
    std::shuffle(order.begin(), order.end(), generator);
    for (std::size_t i = 0; i < count / 2; ++i) {
        pool.deallocate(blocks[order[i]], sizeof(std::uint64_t), alignof(std::uint64_t));
        blocks[order[i]] = nullptr;
    }
    for (std::size_t i = 0; i < count / 2; ++i) {
        auto* block =
            static_cast<std::uint64_t*>(pool.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
        *block = count + i;
    }
    std::size_t survivors = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (blocks[i] != nullptr) {
            CHECK(*blocks[i] == i);
            ++survivors;
        }
    }
    CHECK(survivors == count / 2);
    CHECK(pool.blocks_in_use() == count);
}

void constructor_rejects_impossible_arguments() {
    bool threw = false;
    try {
        memalloc::pool_resource pool(32, 24, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        memalloc::pool_resource pool(32, 8, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        memalloc::pool_resource pool(64, 8, memalloc::detail::size_max);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void doubling_growth_needs_fewer_chunks() {
    memalloc::pool_resource fixed(32, 8, 4, memalloc::pool_growth::fixed);
    memalloc::pool_resource doubling(32, 8, 4, memalloc::pool_growth::doubling);
    for (int i = 0; i < 60; ++i) {
        CHECK(fixed.owns(fixed.allocate(32, 8)));
        CHECK(doubling.owns(doubling.allocate(32, 8)));
    }
    CHECK(fixed.chunk_count() == 15);
    CHECK(fixed.blocks_reserved() == 60);
    CHECK(doubling.chunk_count() == 4);
    CHECK(doubling.blocks_reserved() == 60);
}

void release_returns_every_chunk() {
    counting_resource upstream;
    memalloc::pool_resource pool(64, 16, 8, memalloc::pool_growth::fixed, &upstream);
    std::vector<void*> blocks;
    for (int i = 0; i < 20; ++i) {
        blocks.push_back(pool.allocate(64, 16));
    }
    bool threw = false;
    try {
        pool.release();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(upstream.live_blocks == 3);

    for (void* block : blocks) {
        pool.deallocate(block, 64, 16);
    }
    pool.release();
    CHECK(upstream.live_blocks == 0);
    CHECK(pool.chunk_count() == 0);
    CHECK(pool.blocks_reserved() == 0);
    CHECK(pool.owns(pool.allocate(64, 16)));
    CHECK(pool.chunk_count() == 1);
}

void owns_only_its_own_blocks() {
    memalloc::pool_resource pool(32, 8, 4);
    void* block = pool.allocate(32, 8);
    int outside = 0;
    CHECK(pool.owns(block));
    CHECK(!pool.owns(&outside));
}

void chunks_are_returned_to_upstream() {
    counting_resource upstream;
    {
        memalloc::pool_resource pool(64, 16, 8, memalloc::pool_growth::fixed, &upstream);
        for (int i = 0; i < 20; ++i) {
            CHECK(pool.owns(pool.allocate(64, 16)));
        }
        CHECK(upstream.live_blocks == 3);
        CHECK(upstream.live_bytes == 3 * 64 * 8);
    }
    CHECK(upstream.live_blocks == 0);
    CHECK(upstream.live_bytes == 0);
}

}

int main() {
    return test::run("test_pool", [] {
        blocks_are_sized_and_aligned();
        freed_blocks_come_back_first();
        chunks_grow_on_demand();
        oversized_or_overaligned_requests_throw();
        blocks_never_overlap_under_churn();
        constructor_rejects_impossible_arguments();
        doubling_growth_needs_fewer_chunks();
        release_returns_every_chunk();
        owns_only_its_own_blocks();
        chunks_are_returned_to_upstream();
    });
}
