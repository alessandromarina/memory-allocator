#include <cstdio>
#include <memory_resource>
#include <new>
#include <vector>

#include "memalloc/arena_resource.hpp"
#include "memalloc/pool_resource.hpp"
#include "memalloc/synchronized_resource.hpp"
#include "memalloc/version.hpp"

namespace {

struct particle {
    float position[3];
    float velocity[3];
    float lifetime;
    unsigned id;
};

}

int main() {
    memalloc::arena_resource frame(1 << 16);
    std::pmr::vector<int> scratch(&frame);
    for (int value = 0; value < 100; ++value) {
        scratch.push_back(value);
    }

    auto particles = memalloc::pool_resource::for_type<particle>(64);
    memalloc::synchronized_resource shared(&particles);
    void* storage = shared.allocate(sizeof(particle), alignof(particle));
    auto* one = new (storage) particle{};
    one->id = 7;
    shared.deallocate(storage, sizeof(particle), alignof(particle));

    std::printf("memalloc %s: arena used %zu of %zu, pool reserved %zu blocks of %zu bytes\n",
                MEMALLOC_VERSION_STRING,
                frame.used(),
                frame.capacity(),
                particles.blocks_reserved(),
                particles.block_size());
    return scratch.back() == 99 && particles.blocks_in_use() == 0 ? 0 : 1;
}
