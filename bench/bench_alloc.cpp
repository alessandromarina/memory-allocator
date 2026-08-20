#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <new>
#include <numeric>
#include <random>
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
    std::uint32_t id;
};

constexpr std::size_t block_count = 4096;
constexpr int rounds = 256;
constexpr int repetitions = 9;
constexpr std::size_t locality_count = 1u << 18;
constexpr int locality_passes = 8;
constexpr std::uint32_t seed = 20260820;

std::uint64_t sink = 0;

struct measurement {
    double best;
    double median;
    double worst;
};

measurement measure(void (*body)(), std::size_t operations) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto stop = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::nano>(stop - start).count();
        samples.push_back(elapsed / static_cast<double>(operations));
    }
    std::sort(samples.begin(), samples.end());
    return measurement{samples.front(), samples[samples.size() / 2], samples.back()};
}

void report(const char* label, const measurement& baseline, const measurement& result) {
    std::printf("%-36s %8.2f %8.2f %8.2f  %6.2fx\n",
                label,
                result.median,
                result.best,
                result.worst,
                baseline.median / result.median);
}

void report_header(const char* unit) {
    std::printf("%-36s %8s %8s %8s  %7s\n", unit, "median", "best", "worst", "ratio");
}

class indexed_particles {
public:
    explicit indexed_particles(std::size_t capacity) : storage_(capacity) {
        free_indices_.reserve(capacity);
        for (std::size_t index = capacity; index-- > 0;) {
            free_indices_.push_back(static_cast<std::uint32_t>(index));
        }
    }

    std::uint32_t acquire() {
        const std::uint32_t index = free_indices_.back();
        free_indices_.pop_back();
        return index;
    }

    void release(std::uint32_t index) {
        free_indices_.push_back(index);
    }

    particle& operator[](std::uint32_t index) {
        return storage_[index];
    }

private:
    std::vector<particle> storage_;
    std::vector<std::uint32_t> free_indices_;
};

void touch(particle* target, std::size_t index) {
    target->id = static_cast<std::uint32_t>(index);
    target->lifetime = 1.0f;
    sink += target->id;
}

void churn_new_delete() {
    std::vector<particle*> blocks(block_count);
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t index = 0; index < block_count; ++index) {
            blocks[index] = new particle();
            touch(blocks[index], index);
        }
        for (particle* block : blocks) {
            delete block;
        }
    }
}

template <class Resource>
void churn_resource(Resource& resource) {
    std::vector<void*> blocks(block_count);
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t index = 0; index < block_count; ++index) {
            blocks[index] = resource.allocate(sizeof(particle), alignof(particle));
            touch(new (blocks[index]) particle(), index);
        }
        for (void* block : blocks) {
            resource.deallocate(block, sizeof(particle), alignof(particle));
        }
    }
}

void churn_std_pool() {
    std::pmr::unsynchronized_pool_resource resource;
    churn_resource(resource);
}

void churn_memalloc_pool() {
    auto pool = memalloc::pool_resource::for_type<particle>(block_count);
    churn_resource(pool);
}

void churn_memalloc_pool_synchronized() {
    auto pool = memalloc::pool_resource::for_type<particle>(block_count);
    memalloc::synchronized_resource shared(&pool);
    churn_resource(shared);
}

void churn_indexed_particles() {
    indexed_particles particles(block_count);
    std::vector<std::uint32_t> taken(block_count);
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t index = 0; index < block_count; ++index) {
            taken[index] = particles.acquire();
            touch(&particles[taken[index]], index);
        }
        for (std::uint32_t index : taken) {
            particles.release(index);
        }
    }
}

void frame_monotonic() {
    std::vector<std::byte> storage(block_count * sizeof(particle) * 2);
    for (int round = 0; round < rounds; ++round) {
        std::pmr::monotonic_buffer_resource resource(
            storage.data(), storage.size(), std::pmr::null_memory_resource());
        for (std::size_t index = 0; index < block_count; ++index) {
            touch(new (resource.allocate(sizeof(particle), alignof(particle))) particle(), index);
        }
    }
}

void frame_memalloc_arena() {
    memalloc::arena_resource arena(block_count * sizeof(particle) * 2);
    for (int round = 0; round < rounds; ++round) {
        arena.reset();
        for (std::size_t index = 0; index < block_count; ++index) {
            touch(new (arena.allocate(sizeof(particle), alignof(particle))) particle(), index);
        }
    }
}

std::vector<std::size_t> fragmentation_order(std::size_t count) {
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 generator(seed);
    std::shuffle(order.begin(), order.end(), generator);
    return order;
}

template <class Allocate, class Deallocate>
std::vector<particle*> fragmented_live_set(std::size_t count, Allocate allocate, Deallocate deallocate) {
    std::vector<particle*> blocks(count);
    for (std::size_t index = 0; index < count; ++index) {
        blocks[index] = allocate(index);
    }
    const std::vector<std::size_t> order = fragmentation_order(count);
    for (std::size_t i = 0; i < count / 2; ++i) {
        deallocate(blocks[order[i]]);
        blocks[order[i]] = nullptr;
    }
    for (std::size_t i = 0; i < count / 2; ++i) {
        blocks[order[i]] = allocate(order[i]);
    }
    return blocks;
}

double traverse(const std::vector<particle*>& blocks, int passes) {
    float total = 0.0f;
    for (int pass = 0; pass < passes; ++pass) {
        for (const particle* block : blocks) {
            total += block->lifetime;
        }
    }
    sink += static_cast<std::uint64_t>(total);
    return static_cast<double>(total);
}

double traverse_nanoseconds_per_element(const std::vector<particle*>& blocks) {
    constexpr int passes = locality_passes;
    double best = 0.0;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        traverse(blocks, passes);
        const auto stop = std::chrono::steady_clock::now();
        const double per_element = std::chrono::duration<double, std::nano>(stop - start).count() /
                                   static_cast<double>(blocks.size() * static_cast<std::size_t>(passes));
        if (repetition == 0 || per_element < best) {
            best = per_element;
        }
    }
    return best;
}

void traversal_locality() {
    std::printf("\ntraversal of %zu live objects after fragmentation, ns per element\n", locality_count);

    std::vector<particle*> owned = fragmented_live_set(
        locality_count,
        [](std::size_t index) {
            auto* block = new particle();
            touch(block, index);
            return block;
        },
        [](particle* block) { delete block; });
    const double heap = traverse_nanoseconds_per_element(owned);

    auto pool = memalloc::pool_resource::for_type<particle>(block_count);
    std::vector<particle*> pooled = fragmented_live_set(
        locality_count,
        [&pool](std::size_t index) {
            auto* block = new (pool.allocate(sizeof(particle), alignof(particle))) particle();
            touch(block, index);
            return block;
        },
        [&pool](particle* block) { pool.deallocate(block, sizeof(particle), alignof(particle)); });
    const double pooled_cost = traverse_nanoseconds_per_element(pooled);

    indexed_particles particles(locality_count);
    std::vector<particle*> indexed;
    indexed.reserve(locality_count);
    for (std::size_t index = 0; index < locality_count; ++index) {
        const std::uint32_t slot = particles.acquire();
        touch(&particles[slot], index);
        indexed.push_back(&particles[slot]);
    }
    const double indexed_cost = traverse_nanoseconds_per_element(indexed);

    std::printf("%-36s %8.2f\n", "new / delete", heap);
    std::printf("%-36s %8.2f\n", "memalloc::pool_resource", pooled_cost);
    std::printf("%-36s %8.2f\n", "std::vector + free index list", indexed_cost);

    for (particle* block : owned) {
        delete block;
    }
}

}

int main() {
    const std::size_t operations = static_cast<std::size_t>(rounds) * block_count;

    std::printf("memalloc %s, particle %zu bytes, %zu blocks per round, %d rounds, %d repetitions\n\n",
                MEMALLOC_VERSION_STRING,
                sizeof(particle),
                block_count,
                rounds,
                repetitions);

    std::printf("allocate + touch + free of one fixed-size type, ns per pair\n");
    report_header("resource");
    const measurement churn_baseline = measure(churn_new_delete, operations);
    report("new / delete", churn_baseline, churn_baseline);
    report("std::pmr::unsynchronized_pool", churn_baseline, measure(churn_std_pool, operations));
    report("memalloc::pool_resource", churn_baseline, measure(churn_memalloc_pool, operations));
    report("memalloc::pool + synchronized",
           churn_baseline,
           measure(churn_memalloc_pool_synchronized, operations));
    report("std::vector + free index list", churn_baseline, measure(churn_indexed_particles, operations));

    std::printf("\nallocate for one frame, release in one shot, ns per allocation\n");
    report_header("resource");
    report("new / delete (also pays each free)", churn_baseline, churn_baseline);
    report("std::pmr::monotonic_buffer", churn_baseline, measure(frame_monotonic, operations));
    report("memalloc::arena_resource", churn_baseline, measure(frame_memalloc_arena, operations));

    traversal_locality();

    std::printf("\nchecksum %llu\n", static_cast<unsigned long long>(sink));
    return 0;
}
