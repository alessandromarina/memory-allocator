#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace interleaving {

inline std::function<void(const char*)> preempt;
inline bool preempting = false;

inline void reached(const char* point) {
    if (preempt && !preempting) {
        preempting = true;
        preempt(point);
        preempting = false;
    }
}

}

#define MEMALLOC_INTERLEAVING_POINT(name) interleaving::reached(name)

#include "check.hpp"
#include "memalloc/lock_free_pool_resource.hpp"

namespace {

using pool_type = memalloc::lock_free_pool_resource;

constexpr std::size_t block = 8;

std::size_t slot_for(std::size_t shard) {
    return shard + pool_type::max_shards;
}

void* take(pool_type& pool) {
    return pool.try_allocate(block, block);
}

void give(pool_type& pool, void* address) {
    pool.deallocate(address, block, block);
}

template <class Body>
void run_as(std::size_t slot, Body body) {
    const std::size_t saved = memalloc::detail::thread_slot;
    memalloc::detail::thread_slot = slot;
    body();
    memalloc::detail::thread_slot = saved;
}

bool is(const char* point, const char* expected) {
    return std::strcmp(point, expected) == 0;
}

bool all_distinct(std::vector<void*> blocks) {
    std::sort(blocks.begin(), blocks.end());
    return std::adjacent_find(blocks.begin(), blocks.end()) == blocks.end();
}

bool drains_to_capacity(pool_type& pool) {
    std::vector<void*> blocks;
    while (blocks.size() <= pool.capacity()) {
        void* address = take(pool);
        if (address == nullptr) {
            break;
        }
        blocks.push_back(address);
    }
    const bool complete = blocks.size() == pool.capacity() && all_distinct(blocks);
    for (void* address : blocks) {
        give(pool, address);
    }
    return complete;
}

void the_aba_tag_refuses_a_head_that_came_back() {
    pool_type pool(block, block, 64);
    void* first = nullptr;
    void* stale_next = nullptr;
    bool preempted = false;
    interleaving::preempt = [&](const char* point) {
        if (preempted || !is(point, "pop read the head and its link")) {
            return;
        }
        preempted = true;
        first = take(pool);
        stale_next = take(pool);
        give(pool, first);
    };
    memalloc::detail::thread_slot = slot_for(0);
    void* mine = take(pool);
    interleaving::preempt = nullptr;

    CHECK(preempted);
    CHECK(mine == first);
    void* next = take(pool);
    CHECK(next != nullptr);
    CHECK(next != stale_next);
    CHECK(pool.blocks_in_use() == 3);
}

void exhaustion_is_not_reported_while_a_block_moves_between_shards() {
    pool_type pool(block, block, 128);
    if (pool.shard_count() < 2) {
        std::printf("exhaustion_is_not_reported_while_a_block_moves_between_shards: skipped, one shard\n");
        return;
    }
    std::vector<void*> held;
    run_as(slot_for(0), [&] {
        while (held.size() <= pool.capacity()) {
            void* address = take(pool);
            if (address == nullptr) {
                break;
            }
            held.push_back(address);
        }
    });
    CHECK(held.size() == pool.capacity());
    void* free_in_shard_one = held.back();
    held.pop_back();
    run_as(slot_for(1), [&] { give(pool, free_in_shard_one); });
    void* moving = held.back();
    held.pop_back();

    int empty_shards_seen = 0;
    void* taken_by_other = nullptr;
    interleaving::preempt = [&](const char* point) {
        if (!is(point, "pop found the shard empty") || ++empty_shards_seen != 2) {
            return;
        }
        run_as(slot_for(0), [&] { give(pool, moving); });
        run_as(slot_for(1), [&] { taken_by_other = take(pool); });
    };
    memalloc::detail::thread_slot = slot_for(0);
    void* mine = take(pool);
    interleaving::preempt = nullptr;

    CHECK(empty_shards_seen >= 2);
    CHECK(taken_by_other == free_in_shard_one);
    CHECK(mine == moving);
}

bool platform_reports_the_cpu() {
#if defined(__linux__)
    return pool_type::moves_threads_on_contention && sched_getcpu() >= 0;
#else
    return pool_type::moves_threads_on_contention;
#endif
}

void a_thread_that_loses_a_compare_and_swap_moves_to_its_cpu() {
    constexpr std::size_t parked = 1000;
    pool_type pool(block, block, 64);

    void* taken_by_other = nullptr;
    bool preempted = false;
    interleaving::preempt = [&](const char* point) {
        if (preempted || !is(point, "pop read the head and its link")) {
            return;
        }
        preempted = true;
        taken_by_other = take(pool);
    };
    memalloc::detail::thread_slot = parked;
    void* mine = take(pool);
    interleaving::preempt = nullptr;
    CHECK(preempted);
    CHECK(mine != nullptr && taken_by_other != nullptr && mine != taken_by_other);
    CHECK((memalloc::detail::thread_slot != parked) == platform_reports_the_cpu());

    void* also_taken_by_other = nullptr;
    preempted = false;
    interleaving::preempt = [&](const char* point) {
        if (preempted || !is(point, "push linked the block to the head")) {
            return;
        }
        preempted = true;
        also_taken_by_other = take(pool);
    };
    memalloc::detail::thread_slot = parked;
    give(pool, mine);
    interleaving::preempt = nullptr;
    CHECK(preempted);
    CHECK((memalloc::detail::thread_slot != parked) == platform_reports_the_cpu());
    CHECK(pool.blocks_in_use() == 2);
    give(pool, taken_by_other);
    give(pool, also_taken_by_other);
    CHECK(drains_to_capacity(pool));
}

struct world {
    pool_type pool{block, block, 128};
    std::vector<void*> mine;
    std::vector<void*> theirs;
    std::size_t least_free_during_call = 0;

    std::size_t free_blocks() const {
        return pool.capacity() - mine.size() - theirs.size();
    }

    void observe() {
        least_free_during_call = std::min(least_free_during_call, free_blocks());
    }
};

using script = void (*)(world&);

void allocate_and_free_one(world& w) {
    if (void* address = take(w.pool)) {
        w.theirs.push_back(address);
        w.observe();
        w.theirs.pop_back();
        give(w.pool, address);
    }
}

void allocate_two_and_free_the_first(world& w) {
    void* first = take(w.pool);
    if (first == nullptr) {
        return;
    }
    w.theirs.push_back(first);
    w.observe();
    if (void* second = take(w.pool)) {
        w.theirs.push_back(second);
        w.observe();
    }
    w.theirs.erase(std::find(w.theirs.begin(), w.theirs.end(), first));
    give(w.pool, first);
}

void free_one_held(world& w) {
    if (!w.theirs.empty()) {
        void* address = w.theirs.back();
        w.theirs.pop_back();
        give(w.pool, address);
    }
}

void allocate_one_and_keep_it(world& w) {
    if (void* address = take(w.pool)) {
        w.theirs.push_back(address);
        w.observe();
    }
}

void drain_and_refill(world& w) {
    std::vector<void*> drained;
    while (drained.size() <= w.pool.capacity()) {
        void* address = take(w.pool);
        if (address == nullptr) {
            break;
        }
        drained.push_back(address);
        w.theirs.push_back(address);
        w.observe();
    }
    for (void* address : drained) {
        w.theirs.erase(std::find(w.theirs.begin(), w.theirs.end(), address));
        give(w.pool, address);
    }
}

void move_a_block_to_the_other_shard(world& w) {
    if (w.theirs.empty()) {
        return;
    }
    const std::size_t home = memalloc::detail::thread_slot;
    void* address = w.theirs.back();
    w.theirs.pop_back();
    run_as(home + 1, [&] { give(w.pool, address); });
    allocate_one_and_keep_it(w);
}

struct schedule_result {
    bool ok = true;
    int points = 0;
};

schedule_result run_schedule(script interference, std::size_t their_shard, int preempt_at) {
    world w;
    schedule_result result;
    run_as(slot_for(their_shard), [&] {
        for (int i = 0; i < 4; ++i) {
            w.theirs.push_back(take(w.pool));
        }
    });

    interleaving::preempt = [&](const char*) {
        if (result.points++ == preempt_at) {
            run_as(slot_for(their_shard), [&] { interference(w); });
        }
    };

    const auto my_allocation = [&] {
        if (w.mine.size() + w.theirs.size() > w.pool.capacity()) {
            result.ok = false;
            return false;
        }
        memalloc::detail::thread_slot = slot_for(0);
        w.least_free_during_call = w.free_blocks();
        void* address = take(w.pool);
        if (address == nullptr) {
            result.ok = result.ok && w.least_free_during_call == 0;
            return false;
        }
        w.mine.push_back(address);
        return true;
    };
    const auto my_free = [&](std::size_t position) {
        memalloc::detail::thread_slot = slot_for(0);
        void* address = w.mine[position];
        w.mine.erase(w.mine.begin() + static_cast<std::ptrdiff_t>(position));
        give(w.pool, address);
    };

    for (int i = 0; i < 5; ++i) {
        my_allocation();
    }
    my_free(3);
    my_free(1);
    for (int i = 0; i < 3; ++i) {
        my_allocation();
    }
    while (w.mine.size() > 1) {
        my_free(0);
    }
    while (my_allocation()) {
    }
    for (int i = 0; i < 10 && !w.mine.empty(); ++i) {
        my_free(w.mine.size() / 2);
    }
    my_allocation();
    my_allocation();
    while (!w.mine.empty()) {
        my_free(w.mine.size() - 1);
    }
    interleaving::preempt = nullptr;

    std::vector<void*> everything = w.mine;
    everything.insert(everything.end(), w.theirs.begin(), w.theirs.end());
    result.ok = result.ok && all_distinct(everything) &&
                std::all_of(everything.begin(), everything.end(), [&](void* p) { return w.pool.owns(p); });
    result.ok = result.ok && w.pool.blocks_in_use() == everything.size();
    for (void* address : w.theirs) {
        give(w.pool, address);
    }
    result.ok = result.ok && w.pool.blocks_in_use() == 0 && drains_to_capacity(w.pool);
    return result;
}

void every_single_preemption_keeps_the_pool_consistent() {
    const script scripts[] = {allocate_and_free_one,
                              allocate_two_and_free_the_first,
                              free_one_held,
                              allocate_one_and_keep_it,
                              drain_and_refill,
                              move_a_block_to_the_other_shard};
    const char* names[] = {"allocate and free one",
                           "allocate two and free the first",
                           "free one held",
                           "allocate one and keep it",
                           "drain and refill",
                           "move a block to the other shard"};
    const int points_without_preemption = run_schedule(free_one_held, 0, -1).points;
    CHECK(points_without_preemption > 0);
    int schedules = 0;
    int failures = 0;
    for (std::size_t s = 0; s < sizeof(scripts) / sizeof(scripts[0]); ++s) {
        for (std::size_t their_shard = 0; their_shard < 2; ++their_shard) {
            for (int at = 0; at < points_without_preemption + 64; ++at) {
                ++schedules;
                if (!run_schedule(scripts[s], their_shard, at).ok && failures++ == 0) {
                    std::printf("first inconsistent schedule: \"%s\" from shard %zu at point %d\n",
                                names[s],
                                their_shard,
                                at);
                }
            }
        }
    }
    std::printf("explored %d schedules, %d interleaving points each without preemption\n",
                schedules,
                points_without_preemption);
    CHECK(failures == 0);
}

}

int main() {
    return test::run("test_lock_free_interleaving", [] {
        the_aba_tag_refuses_a_head_that_came_back();
        exhaustion_is_not_reported_while_a_block_moves_between_shards();
        a_thread_that_loses_a_compare_and_swap_moves_to_its_cpu();
        every_single_preemption_keeps_the_pool_consistent();
    });
}
