#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "check.hpp"
#include "memalloc/pool_resource.hpp"

namespace {

struct payload {
    std::uint64_t tag;
    std::uint64_t filler[3];
};

struct live_block {
    payload* address;
    std::uint64_t tag;
};

bool addresses_are_unique(std::vector<live_block> blocks) {
    std::sort(blocks.begin(), blocks.end(), [](const live_block& left, const live_block& right) {
        return left.address < right.address;
    });
    return std::adjacent_find(
               blocks.begin(), blocks.end(), [](const live_block& left, const live_block& right) {
                   return left.address == right.address;
               }) == blocks.end();
}

void random_sequence_matches_the_oracle(std::uint32_t seed, memalloc::pool_growth growth) {
    constexpr int steps = 20000;
    memalloc::pool_resource pool(sizeof(payload), alignof(payload), 16, growth);
    std::mt19937 generator(seed);
    std::bernoulli_distribution allocates(0.55);

    std::vector<live_block> live;
    std::uint64_t next_tag = 1;
    std::size_t peak = 0;

    for (int step = 0; step < steps; ++step) {
        if (live.empty() || allocates(generator)) {
            auto* address = static_cast<payload*>(pool.allocate(sizeof(payload), alignof(payload)));
            CHECK(pool.owns(address));
            address->tag = next_tag;
            live.push_back(live_block{address, next_tag});
            ++next_tag;
            peak = std::max(peak, live.size());
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t index = pick(generator);
            CHECK(live[index].address->tag == live[index].tag);
            pool.deallocate(live[index].address, sizeof(payload), alignof(payload));
            live[index] = live.back();
            live.pop_back();
        }
        CHECK(pool.blocks_in_use() == live.size());
        CHECK(pool.blocks_reserved() >= live.size());
    }

    CHECK(addresses_are_unique(live));
    for (const live_block& block : live) {
        CHECK(block.address->tag == block.tag);
    }
    CHECK(peak > 0);
    CHECK(pool.blocks_reserved() >= peak);

    for (const live_block& block : live) {
        pool.deallocate(block.address, sizeof(payload), alignof(payload));
    }
    CHECK(pool.blocks_in_use() == 0);
    pool.release();
    CHECK(pool.chunk_count() == 0);
}

}

int main() {
    for (std::uint32_t seed : {1u, 20260820u, 4294967291u}) {
        random_sequence_matches_the_oracle(seed, memalloc::pool_growth::fixed);
        random_sequence_matches_the_oracle(seed, memalloc::pool_growth::doubling);
    }
    if (test::failures == 0) {
        std::printf("test_pool_property: all checks passed\n");
    }
    return test::failures == 0 ? 0 : 1;
}
