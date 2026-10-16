#include "memalloc/lock_free_pool_resource.hpp"

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <stdexcept>
#include <thread>
#include <vector>

#include "check.hpp"

namespace {

constexpr int thread_count = 4;

class counting_resource : public std::pmr::memory_resource {
public:
    std::size_t live_blocks = 0;

protected:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++live_blocks;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        --live_blocks;
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

bool is_aligned(const void* p, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}

template <class Exception, class Body>
bool throws(Body body) {
    try {
        body();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

void the_head_is_lock_free() {
    memalloc::lock_free_pool_resource pool(16, 8, 4);
    CHECK(pool.is_lock_free());
}

void blocks_are_sized_and_aligned() {
    memalloc::lock_free_pool_resource pool(1, 1, 4);
    CHECK(pool.block_size() == 1);
    memalloc::lock_free_pool_resource wide(24, 64, 4);
    CHECK(wide.block_size() == 64);
    for (int i = 0; i < 4; ++i) {
        CHECK(is_aligned(wide.allocate(24, 64), 64));
    }
}

void the_free_list_lives_outside_the_blocks() {
    auto pool = memalloc::lock_free_pool_resource::for_type<std::uint32_t>(8);
    CHECK(pool.block_size() == sizeof(std::uint32_t));
    auto* first = static_cast<std::uint32_t*>(pool.allocate(4, 4));
    auto* second = static_cast<std::uint32_t*>(pool.allocate(4, 4));
    CHECK(second == first + 1);
}

void freed_blocks_come_back_first() {
    memalloc::lock_free_pool_resource pool(32, 8, 8);
    void* a = pool.allocate(32, 8);
    void* b = pool.allocate(32, 8);
    pool.deallocate(b, 32, 8);
    pool.deallocate(a, 32, 8);
    CHECK(pool.blocks_in_use() == 0);
    CHECK(pool.allocate(32, 8) == a);
    CHECK(pool.allocate(32, 8) == b);
}

void exhaustion_throws_and_recovers() {
    memalloc::lock_free_pool_resource pool(32, 8, 4);
    std::vector<void*> blocks;
    for (int i = 0; i < 4; ++i) {
        blocks.push_back(pool.allocate(32, 8));
    }
    CHECK(pool.blocks_in_use() == 4);
    CHECK_THROWS_BAD_ALLOC(pool.allocate(32, 8));
    pool.deallocate(blocks.back(), 32, 8);
    CHECK(pool.allocate(32, 8) == blocks.back());
}

void every_capacity_at_a_sentinel_boundary_works() {
    for (std::size_t capacity : {1u, 2u, 3u, 4u, 7u, 8u, 255u, 256u, 4094u, 4095u, 4096u}) {
        memalloc::lock_free_pool_resource pool(8, 8, capacity);
        std::vector<void*> blocks;
        for (std::size_t i = 0; i < capacity; ++i) {
            blocks.push_back(pool.allocate(8, 8));
        }
        CHECK(pool.blocks_in_use() == capacity);
        CHECK_THROWS_BAD_ALLOC(pool.allocate(8, 8));
        for (void* block : blocks) {
            pool.deallocate(block, 8, 8);
        }
        CHECK(pool.blocks_in_use() == 0);
        CHECK(pool.allocate(8, 8) == blocks.back());
    }
}

void the_aba_tag_takes_every_bit_the_index_does_not_need() {
    CHECK(memalloc::lock_free_pool_resource(8, 8, 1).aba_tag_bits() == 62);
    CHECK(memalloc::lock_free_pool_resource(8, 8, 4094).aba_tag_bits() == 52);
    CHECK(memalloc::lock_free_pool_resource(8, 8, 4095).aba_tag_bits() == 51);
    CHECK(memalloc::lock_free_pool_resource(8, 8, 1u << 20).aba_tag_bits() == 43);
}

void shards_are_a_power_of_two_and_never_too_small() {
    for (std::size_t capacity : {1u, 63u, 64u, 127u, 128u, 1000u, 4096u, 1u << 20}) {
        memalloc::lock_free_pool_resource pool(8, 8, capacity);
        const std::size_t shards = pool.shard_count();
        CHECK(shards >= 1);
        CHECK(shards <= memalloc::lock_free_pool_resource::max_shards);
        CHECK((shards & (shards - 1)) == 0);
        CHECK(shards == 1 || capacity / shards >= memalloc::lock_free_pool_resource::min_blocks_per_shard);
    }
}

void one_thread_reaches_every_shard() {
    memalloc::lock_free_pool_resource pool(8, 8, 4096);
    std::vector<void*> blocks;
    for (std::size_t i = 0; i < 4096; ++i) {
        blocks.push_back(pool.allocate(8, 8));
    }
    std::sort(blocks.begin(), blocks.end());
    CHECK(std::adjacent_find(blocks.begin(), blocks.end()) == blocks.end());
    CHECK_THROWS_BAD_ALLOC(pool.allocate(8, 8));
    for (void* block : blocks) {
        pool.deallocate(block, 8, 8);
    }
    CHECK(pool.blocks_in_use() == 0);
}

void exhaustion_is_never_reported_while_a_block_is_free() {
    constexpr std::size_t slots = 512;
    constexpr int cycles = 50000;
    memalloc::lock_free_pool_resource pool(16, 8, slots + thread_count);
    std::vector<std::atomic<void*>> ring(slots);
    std::atomic<int> false_exhaustions{0};
    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &pool, &ring, &false_exhaustions] {
            std::size_t position = static_cast<std::size_t>(id) * 131;
            for (int cycle = 0; cycle < cycles; ++cycle) {
                void* block = nullptr;
                try {
                    block = pool.allocate(16, 8);
                } catch (const std::bad_alloc&) {
                    false_exhaustions.fetch_add(1);
                    continue;
                }
                position = (position + 97) % slots;
                if (void* previous = ring[position].exchange(block, std::memory_order_acq_rel)) {
                    pool.deallocate(previous, 16, 8);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    for (std::atomic<void*>& slot : ring) {
        if (void* left = slot.exchange(nullptr)) {
            pool.deallocate(left, 16, 8);
        }
    }
    CHECK(false_exhaustions.load() == 0);
    CHECK(pool.blocks_in_use() == 0);
}

void try_allocate_returns_null_instead_of_throwing() {
    memalloc::lock_free_pool_resource pool(32, 8, 4);
    CHECK(pool.try_allocate(33, 8) == nullptr);
    CHECK(pool.try_allocate(32, 16) == nullptr);
    std::vector<void*> blocks;
    for (int i = 0; i < 4; ++i) {
        blocks.push_back(pool.try_allocate(32, 8));
        CHECK(pool.owns(blocks.back()));
    }
    CHECK(pool.try_allocate(32, 8) == nullptr);
    CHECK(pool.blocks_in_use() == 4);
    pool.deallocate(blocks[2], 32, 8);
    CHECK(pool.try_allocate(32, 8) == blocks[2]);
    for (void* block : blocks) {
        pool.deallocate(block, 32, 8);
    }
    CHECK(pool.blocks_in_use() == 0);
}

void concurrent_draining_with_try_allocate_hands_out_every_block_once() {
    constexpr std::size_t capacity = 4096;
    memalloc::lock_free_pool_resource pool(16, 8, capacity);

    std::vector<std::vector<void*>> taken(thread_count);
    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &pool, &taken] {
            auto& mine = taken[static_cast<std::size_t>(id)];
            mine.reserve(capacity);
            while (void* block = pool.try_allocate(16, 8)) {
                mine.push_back(block);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    std::vector<void*> all;
    for (const auto& mine : taken) {
        all.insert(all.end(), mine.begin(), mine.end());
    }
    std::sort(all.begin(), all.end());
    CHECK(all.size() == capacity);
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
}

void oversized_or_overaligned_requests_throw() {
    memalloc::lock_free_pool_resource pool(32, 8, 4);
    CHECK_THROWS_BAD_ALLOC(pool.allocate(33, 8));
    CHECK_THROWS_BAD_ALLOC(pool.allocate(32, 16));
    CHECK(pool.blocks_in_use() == 0);
}

void constructor_rejects_impossible_arguments() {
    CHECK(throws<std::invalid_argument>([] { memalloc::lock_free_pool_resource pool(32, 24, 4); }));
    CHECK(throws<std::invalid_argument>([] { memalloc::lock_free_pool_resource pool(32, 8, 0); }));
    CHECK(throws<std::invalid_argument>(
        [] { memalloc::lock_free_pool_resource pool(64, 8, memalloc::detail::size_max); }));
    CHECK(throws<std::invalid_argument>([] {
        memalloc::lock_free_pool_resource pool(
            memalloc::detail::size_max / 2, 8, memalloc::lock_free_pool_resource::max_capacity);
    }));
    CHECK(throws<std::invalid_argument>([] { memalloc::lock_free_pool_resource pool(32, 8, 4, nullptr); }));
}

void storage_is_one_upstream_allocation() {
    counting_resource upstream;
    {
        memalloc::lock_free_pool_resource pool(64, 16, 128, &upstream);
        CHECK(upstream.live_blocks == 1);
        CHECK(pool.owns(pool.allocate(64, 16)));
    }
    CHECK(upstream.live_blocks == 0);
}

void owns_only_its_own_blocks() {
    memalloc::lock_free_pool_resource pool(32, 8, 4);
    void* block = pool.allocate(32, 8);
    int outside = 0;
    CHECK(pool.owns(block));
    CHECK(!pool.owns(&outside));
    CHECK(!pool.owns(static_cast<std::byte*>(block) + 4 * 32));
}

void concurrent_churn_never_shares_a_block() {
    constexpr int cycles = 20000;
    constexpr std::size_t held = 8;
    auto pool = memalloc::lock_free_pool_resource::for_type<std::uint64_t>(thread_count * held);

    std::atomic<int> corruptions{0};
    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &pool, &corruptions] {
            const auto pattern = static_cast<std::uint64_t>(0x5eed'0000u) + static_cast<std::uint64_t>(id);
            std::uint64_t* mine[held] = {};
            for (int cycle = 0; cycle < cycles; ++cycle) {
                const auto slot = static_cast<std::size_t>(cycle) % held;
                if (mine[slot] != nullptr) {
                    if (*mine[slot] != pattern + static_cast<std::uint64_t>(slot)) {
                        corruptions.fetch_add(1);
                    }
                    pool.deallocate(mine[slot], sizeof(std::uint64_t), alignof(std::uint64_t));
                }
                mine[slot] =
                    static_cast<std::uint64_t*>(pool.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
                *mine[slot] = pattern + static_cast<std::uint64_t>(slot);
            }
            for (std::uint64_t* block : mine) {
                pool.deallocate(block, sizeof(std::uint64_t), alignof(std::uint64_t));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(corruptions.load() == 0);
    CHECK(pool.blocks_in_use() == 0);
}

void concurrent_draining_hands_out_every_block_once() {
    constexpr std::size_t capacity = 4096;
    memalloc::lock_free_pool_resource pool(16, 8, capacity);

    std::vector<std::vector<void*>> taken(thread_count);
    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &pool, &taken] {
            auto& mine = taken[static_cast<std::size_t>(id)];
            mine.reserve(capacity);
            try {
                for (;;) {
                    mine.push_back(pool.allocate(16, 8));
                }
            } catch (const std::bad_alloc&) {
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    std::vector<void*> all;
    for (const auto& mine : taken) {
        all.insert(all.end(), mine.begin(), mine.end());
    }
    std::sort(all.begin(), all.end());
    CHECK(all.size() == capacity);
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    CHECK(pool.blocks_in_use() == capacity);
}

void blocks_freed_by_another_thread_are_reused() {
    constexpr std::size_t per_thread = 1024;
    memalloc::lock_free_pool_resource pool(32, 8, thread_count * per_thread);

    std::vector<std::vector<void*>> batches(thread_count);
    for (auto& batch : batches) {
        for (std::size_t i = 0; i < per_thread; ++i) {
            batch.push_back(pool.allocate(32, 8));
        }
    }
    CHECK_THROWS_BAD_ALLOC(pool.allocate(32, 8));

    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &pool, &batches] {
            auto& theirs = batches[static_cast<std::size_t>((id + 1) % thread_count)];
            for (void* block : theirs) {
                pool.deallocate(block, 32, 8);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(pool.blocks_in_use() == 0);
    for (std::size_t i = 0; i < thread_count * per_thread; ++i) {
        CHECK(pool.owns(pool.allocate(32, 8)));
    }
}

void works_as_a_container_resource() {
    memalloc::lock_free_pool_resource pool(64, alignof(std::max_align_t), 256);
    {
        std::pmr::vector<int> one(&pool);
        one.push_back(42);
        CHECK(one.front() == 42);
        CHECK(pool.blocks_in_use() >= 1);
    }
    CHECK(pool.blocks_in_use() == 0);
}

}

int main() {
    return test::run("test_lock_free_pool", [] {
        the_head_is_lock_free();
        blocks_are_sized_and_aligned();
        the_free_list_lives_outside_the_blocks();
        freed_blocks_come_back_first();
        exhaustion_throws_and_recovers();
        every_capacity_at_a_sentinel_boundary_works();
        the_aba_tag_takes_every_bit_the_index_does_not_need();
        shards_are_a_power_of_two_and_never_too_small();
        one_thread_reaches_every_shard();
        oversized_or_overaligned_requests_throw();
        try_allocate_returns_null_instead_of_throwing();
        constructor_rejects_impossible_arguments();
        storage_is_one_upstream_allocation();
        owns_only_its_own_blocks();
        concurrent_churn_never_shares_a_block();
        concurrent_draining_hands_out_every_block_once();
        concurrent_draining_with_try_allocate_hands_out_every_block_once();
        blocks_freed_by_another_thread_are_reused();
        exhaustion_is_never_reported_while_a_block_is_free();
        works_as_a_container_resource();
    });
}
