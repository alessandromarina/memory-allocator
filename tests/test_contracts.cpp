#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "check.hpp"
#include "memalloc/arena_resource.hpp"
#include "memalloc/pool_resource.hpp"

namespace {

struct violation : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void throw_on_violation(const char* message) {
    throw violation(message);
}

template <class Body>
bool reports_a_violation(Body body) {
    const memalloc::violation_handler previous = memalloc::set_violation_handler(&throw_on_violation);
    bool reported = false;
    try {
        body();
    } catch (const violation&) {
        reported = true;
    }
    memalloc::set_violation_handler(previous);
    return reported;
}

void pool_rejects_a_foreign_pointer() {
    memalloc::pool_resource pool(32, 8, 4);
    std::uint64_t outside = 0;
    CHECK(reports_a_violation([&pool, &outside] { pool.deallocate(&outside, 32, 8); }));
}

void pool_rejects_a_pointer_inside_a_block() {
    memalloc::pool_resource pool(32, 8, 4);
    auto* block = static_cast<std::byte*>(pool.allocate(32, 8));
    CHECK(reports_a_violation([&pool, block] { pool.deallocate(block + 8, 32, 8); }));
}

void pool_rejects_one_free_too_many() {
    memalloc::pool_resource pool(32, 8, 4);
    void* block = pool.allocate(32, 8);
    pool.deallocate(block, 32, 8);
    CHECK(reports_a_violation([&pool, block] { pool.deallocate(block, 32, 8); }));
}

void pool_rejects_a_double_free_while_other_blocks_live() {
    memalloc::pool_resource pool(32, 8, 8);
    void* first = pool.allocate(32, 8);
    void* second = pool.allocate(32, 8);
    pool.deallocate(first, 32, 8);
    CHECK(reports_a_violation([&pool, first] { pool.deallocate(first, 32, 8); }));
    pool.deallocate(second, 32, 8);
}

void arena_rejects_a_foreign_pointer() {
    memalloc::arena_resource arena(64);
    std::uint64_t outside = 0;
    CHECK(reports_a_violation([&arena, &outside] { arena.deallocate(&outside, 8, 8); }));
}

void the_handler_can_be_restored() {
    const memalloc::violation_handler previous = memalloc::set_violation_handler(&throw_on_violation);
    CHECK(memalloc::set_violation_handler(previous) == &throw_on_violation);
}

}

int main() {
    pool_rejects_a_foreign_pointer();
    pool_rejects_a_pointer_inside_a_block();
    pool_rejects_one_free_too_many();
    pool_rejects_a_double_free_while_other_blocks_live();
    arena_rejects_a_foreign_pointer();
    the_handler_can_be_restored();
    if (test::failures == 0) {
        std::printf("test_contracts: all checks passed\n");
    }
    return test::failures == 0 ? 0 : 1;
}
