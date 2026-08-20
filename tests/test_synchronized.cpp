#include "memalloc/synchronized_resource.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <thread>
#include <vector>

#include "check.hpp"
#include "memalloc/arena_resource.hpp"
#include "memalloc/pool_resource.hpp"

namespace {

constexpr int thread_count = 4;
constexpr int cycles_per_thread = 4000;

void shared_pool_survives_concurrent_churn() {
    auto pool = memalloc::pool_resource::for_type<std::uint64_t>(256);
    memalloc::synchronized_resource shared(&pool);

    std::vector<int> corruptions(thread_count, 0);
    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &shared, &corruptions] {
            const auto pattern = static_cast<std::uint64_t>(0x5eed'0000u) + static_cast<std::uint64_t>(id);
            for (int cycle = 0; cycle < cycles_per_thread; ++cycle) {
                auto* block = static_cast<std::uint64_t*>(
                    shared.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
                *block = pattern;
                if (*block != pattern) {
                    ++corruptions[static_cast<std::size_t>(id)];
                }
                shared.deallocate(block, sizeof(std::uint64_t), alignof(std::uint64_t));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    for (int count : corruptions) {
        CHECK(count == 0);
    }
    CHECK(pool.blocks_in_use() == 0);
    CHECK(pool.chunk_count() >= 1);
}

void blocks_handed_out_concurrently_never_overlap() {
    auto pool = memalloc::pool_resource::for_type<std::uint64_t>(64);
    memalloc::synchronized_resource shared(&pool);

    constexpr std::size_t per_thread = 512;
    std::vector<std::vector<std::uint64_t*>> taken(thread_count);
    std::vector<std::thread> workers;
    for (int id = 0; id < thread_count; ++id) {
        workers.emplace_back([id, &shared, &taken] {
            auto& mine = taken[static_cast<std::size_t>(id)];
            mine.reserve(per_thread);
            for (std::size_t i = 0; i < per_thread; ++i) {
                mine.push_back(static_cast<std::uint64_t*>(
                    shared.allocate(sizeof(std::uint64_t), alignof(std::uint64_t))));
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    std::vector<std::uint64_t*> all;
    for (const auto& mine : taken) {
        all.insert(all.end(), mine.begin(), mine.end());
    }
    std::sort(all.begin(), all.end());
    CHECK(all.size() == thread_count * per_thread);
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    CHECK(pool.blocks_in_use() == thread_count * per_thread);
}

void containers_work_through_the_wrapper() {
    memalloc::arena_resource arena(256 * 1024);
    memalloc::synchronized_resource shared(&arena);
    std::pmr::vector<int> values(&shared);
    for (int i = 0; i < 1000; ++i) {
        values.push_back(i);
    }
    CHECK(values.back() == 999);
    CHECK(shared.wrapped() == &arena);
    CHECK(arena.used() > 0);
}

}

int main() {
    return test::run("test_synchronized", [] {
        shared_pool_survives_concurrent_churn();
        blocks_handed_out_concurrently_never_overlap();
        containers_work_through_the_wrapper();
    });
}
