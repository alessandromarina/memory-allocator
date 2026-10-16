#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <thread>
#include <vector>

#include "check.hpp"
#include "memalloc/arena_resource.hpp"
#include "memalloc/lock_free_pool_resource.hpp"
#include "memalloc/pool_resource.hpp"
#include "memalloc/synchronized_resource.hpp"

static_assert(MEMALLOC_HAS_EXCEPTIONS == 0, "test_no_exceptions must be compiled with exceptions disabled");

namespace {

constexpr int thread_count = 4;

const char* expected_report = "";

[[noreturn]] void exit_on_expected_report(const char* message) {
    const bool expected = std::strstr(message, expected_report) != nullptr;
    std::printf("test_no_exceptions: reported \"%s\"\n", message);
    std::printf("test_no_exceptions: %s\n", expected ? "all checks passed" : "unexpected report");
    std::fflush(stdout);
    std::_Exit(expected ? 0 : 1);
}

void arena_try_allocate_reports_exhaustion_with_null() {
    memalloc::arena_resource arena(1024, 8);
    CHECK(arena.try_allocate(1024, 8) != nullptr);
    CHECK(arena.try_allocate(1, 1) == nullptr);
    CHECK(arena.try_allocate(1, 16) == nullptr);
    arena.reset();
    std::pmr::vector<int> values(&arena);
    for (int i = 0; i < 8; ++i) {
        values.push_back(i);
    }
    CHECK(values.back() == 7);
}

void pool_try_allocate_grows_and_refuses_what_it_cannot_serve() {
    memalloc::pool_resource pool(32, 8, 4);
    CHECK(pool.try_allocate(33, 8) == nullptr);
    std::vector<void*> blocks;
    for (int i = 0; i < 9; ++i) {
        blocks.push_back(pool.try_allocate(32, 8));
        CHECK(pool.owns(blocks.back()));
    }
    CHECK(pool.chunk_count() == 3);
    for (void* block : blocks) {
        pool.deallocate(block, 32, 8);
    }
    pool.release();
    CHECK(pool.chunk_count() == 0);
}

void lock_free_try_allocate_reports_exhaustion_with_null() {
    memalloc::lock_free_pool_resource pool(16, 8, 256);
    std::vector<void*> blocks;
    while (void* block = pool.try_allocate(16, 8)) {
        blocks.push_back(block);
    }
    CHECK(blocks.size() == 256);
    CHECK(pool.blocks_in_use() == 256);
    pool.deallocate(blocks.front(), 16, 8);
    CHECK(pool.try_allocate(16, 8) == blocks.front());
    for (void* block : blocks) {
        pool.deallocate(block, 16, 8);
    }
    CHECK(pool.blocks_in_use() == 0);
}

void lock_free_concurrent_draining_hands_out_every_block_once() {
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

void synchronized_resource_works_without_exceptions() {
    memalloc::pool_resource pool(32, 8, 4);
    memalloc::synchronized_resource shared(&pool);
    void* block = shared.allocate(32, 8);
    CHECK(pool.owns(block));
    shared.deallocate(block, 32, 8);
    CHECK(pool.blocks_in_use() == 0);
}

int run_failure(const char* name) {
    memalloc::set_violation_handler(&exit_on_expected_report);
    if (std::strcmp(name, "lock_free_exhaustion") == 0) {
        expected_report = "lock_free_pool_resource::allocate failed with exceptions disabled";
        memalloc::lock_free_pool_resource pool(16, 8, 1);
        (void)pool.allocate(16, 8);
        (void)pool.allocate(16, 8);
    } else if (std::strcmp(name, "arena_exhaustion") == 0) {
        expected_report = "arena_resource::allocate failed with exceptions disabled";
        memalloc::arena_resource arena(16);
        (void)arena.allocate(32, 8);
    } else if (std::strcmp(name, "pool_oversized") == 0) {
        expected_report = "pool_resource::allocate of a size or alignment it cannot serve";
        memalloc::pool_resource pool(16, 8, 4);
        (void)pool.allocate(32, 8);
    } else if (std::strcmp(name, "invalid_argument") == 0) {
        expected_report = "memalloc::lock_free_pool_resource: capacity is zero";
        memalloc::lock_free_pool_resource pool(16, 8, 0);
    } else if (std::strcmp(name, "release_with_live_blocks") == 0) {
        expected_report = "memalloc::pool_resource::release with live blocks";
        memalloc::pool_resource pool(16, 8, 4);
        (void)pool.allocate(16, 8);
        pool.release();
    } else {
        std::printf("test_no_exceptions: unknown failure case %s\n", name);
        return 2;
    }
    std::printf("test_no_exceptions: %s returned instead of reporting\n", name);
    return 1;
}

}

int main(int argc, char** argv) {
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc > 1) {
        return run_failure(argv[1]);
    }
    return test::run("test_no_exceptions", [] {
        arena_try_allocate_reports_exhaustion_with_null();
        pool_try_allocate_grows_and_refuses_what_it_cannot_serve();
        lock_free_try_allocate_reports_exhaustion_with_null();
        lock_free_concurrent_draining_hands_out_every_block_once();
        synchronized_resource_works_without_exceptions();
    });
}
