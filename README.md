# memory-allocator - fixed-size pool and frame arena for C++

[![ci](https://img.shields.io/github/actions/workflow/status/alessandromarina/memory-allocator/ci.yml?branch=main&style=flat-square&label=ci)](https://github.com/alessandromarina/memory-allocator/actions/workflows/ci.yml)
[![license](https://img.shields.io/github/license/alessandromarina/memory-allocator?style=flat-square)](LICENSE)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?style=flat-square&logo=cmake&logoColor=white)

Allocators for games and other soft real-time code, exposed as `std::pmr::memory_resource`: a pool
for one fixed-size type, a linear arena reset once per frame, a wrapper that makes either of them
thread safe behind a mutex, and a lock-free pool for blocks shared between threads. Header only,
C++17, no dependencies.

- Language: C++17, header only
- Build: CMake 3.16 or newer
- Version: 0.4.0
- License: Apache-2.0

## Author

- Alessandro Marina (AlessandroMarinaPode@gmail.com)

## Implementation status

Legend:

- 🟢: Implemented
- 🟡: Work in progress
- 🔴: Not implemented

| Feature                                             | Status |
|-----------------------------------------------------|--------|
| `pool_resource`, fixed block, intrusive free list    | 🟢     |
| Lazy chunk carving, no page touching on growth       | 🟢     |
| `arena_resource`, bump pointer with reset            | 🟢     |
| `synchronized_resource`, mutex wrapper               | 🟢     |
| Fixed or doubling chunk growth                       | 🟢     |
| Argument validation on construction                  | 🟢     |
| Contract checks in debug builds, custom handler       | 🟢     |
| Randomised property test against an oracle           | 🟢     |
| Benchmark, median and spread, against three rivals   | 🟢     |
| CI on gcc, clang, MSVC, Apple clang, with sanitizers | 🟢     |
| `install` and `find_package` support                 | 🟢     |
| `lock_free_pool_resource`, sharded Treiber stacks    | 🟢     |

## Overview

Games allocate in two shapes. Many objects of the same type, created and destroyed continuously
(particles, projectiles, event nodes), and a lot of scratch data that lives exactly one frame and
then dies all at once. The general purpose allocator behind `new` has to serve every shape, so it
pays for size classes, coalescing and thread safety on every call.

`pool_resource` serves one block shape. Free blocks are threaded through a singly linked list stored
in the blocks themselves, so allocate is a pointer read and deallocate is a pointer write. Memory is
carved from chunks taken from an upstream resource, and every chunk is returned to it in the
destructor or in `release()`. A new chunk is carved lazily, one block per allocation, so growing the
pool costs one upstream call and never walks or touches the whole chunk: on a 65536 block chunk the
allocation that triggers growth dropped from about 630 microseconds to about 3.

`arena_resource` owns one buffer and a cursor. Allocation aligns the cursor and moves it forward,
deallocation does nothing, and `reset()` sets the cursor back to zero. There is no per-object
bookkeeping at all, which is also why it demands the most discipline: `reset()` invalidates every
pointer handed out and calls no destructor.

`lock_free_pool_resource` is the pool for blocks that several threads allocate and free at the same
time. It owns one buffer of a fixed number of blocks, split into up to 16 shards, each a Treiber stack
on its own cache line. A thread allocates from and frees into the shard its thread slot picks, and
only when that shard is empty does it steal from the others, so threads that mostly reuse their own
blocks never touch the same cache line. Each head packs a block index and an ABA tag into one 64 bit
atomic, so no double width compare and swap is needed; the index takes only the bits the capacity
needs and the tag gets all the others: 51 bits for 4096 blocks, never fewer than 32. The links live in
a separate array of atomics rather than inside the blocks: a thread that loses a race never reads
memory that another thread is already writing to, which keeps the type free of data races and clean
under ThreadSanitizer.

All of them derive from `std::pmr::memory_resource`, so they work with `std::pmr` containers and with
any code that takes a `memory_resource*`, without templates leaking into the call sites.

## Design decisions

**The lock is a decorator, not a flag.** Neither allocator takes a lock, because most of the time
there is nothing to lock: one resource per thread is the arrangement that makes them fast. When a
resource really has to be shared, `synchronized_resource` wraps any other resource and serialises
`allocate` and `deallocate` behind a mutex. Keeping the lock outside means the single threaded path
stays exactly as it was, and the price of sharing is visible in the type at the call site.

**Lock-free, and sharded rather than cached.** `lock_free_pool_resource` never blocks: a thread
that is preempted in the middle of an allocation cannot stall the others, which is what an audio
callback, a job system worker or a signal handler needs. A single shared free list would make every
call a compare and swap on one cache line, and in a first version that made the pool slower than
`new` under contention. Per thread caches would fix that, but they need state tied to both a thread
and a resource, and blocks parked in one thread's cache are invisible to the others. Shards keep the
benefit without either problem: the only per thread state is a global slot number, every free block
sits in some shard where any thread can steal it, and exhaustion is reported only after a second pass
finds every shard head unchanged, so `std::bad_alloc` still means that, at one instant, no block was
free.

**No resource is copyable or movable**, exactly like `std::pmr::monotonic_buffer_resource`. A
`memory_resource` hands its address to every container built on it, so an object that can be moved out
from under those containers is a dangling pointer waiting to happen. Address stability is the feature.
To keep several of them, use `std::deque<pool_resource>` with `emplace_back`, which never moves its
elements, or `std::vector<std::unique_ptr<pool_resource>>`.

**Exhaustion is an error, not a growth event.** The arena has a fixed capacity and throws
`std::bad_alloc` instead of falling back upstream. A frame budget that silently grows is a stutter
later, in a build where nobody is looking. `high_water_mark()` is there to size the budget once, from
a real run.

**Contract violations are reported, not ignored.** In debug builds the pool refuses a pointer it never
handed out, a pointer that is not on a block boundary, a size or alignment it could not have served,
and one free too many; with
`MEMALLOC_DEBUG_SLOW_CHECKS=1` it also scans the free list and catches a double free while other
blocks are live. The lock-free pool marks every block it hands out in its link array, so it catches a
double free in constant time, without the slow checks. All of it compiles away under `NDEBUG`, and
none of it changes the object layout, so a debug and a release translation unit still agree on what
these classes look like.

## Why not std::pmr::unsynchronized_pool_resource

Because the standard resources are general, and generality costs. `unsynchronized_pool_resource`
keeps several size classes, picks one per request, and grows its chunks geometrically;
`monotonic_buffer_resource` keeps a chain of buffers and can fall back upstream. Both are good
defaults, neither can assume what these two can: exactly one block size, and exactly one buffer with
a fixed budget.

The honest version in one line: against the standard resources the gain is a modest constant factor,
against global `new` and `delete` it is a large one. On libstdc++, `unsynchronized_pool_resource` was
in fact slower than `new` in the benchmark below.

## Usage

Pool, one type, manual lifetime:

```cpp
#include "memalloc/pool_resource.hpp"

struct particle {
    float position[3];
    float velocity[3];
    float lifetime;
    std::uint32_t id;
};

auto pool = memalloc::pool_resource::for_type<particle>(4096);

void* storage = pool.allocate(sizeof(particle), alignof(particle));
auto* p = new (storage) particle{};

p->~particle();
pool.deallocate(storage, sizeof(particle), alignof(particle));
```

`for_type<T>` returns by value and the type is not movable: this compiles because C++17 guarantees
copy elision for a prvalue initialiser, and it is the only supported form.

Chunk growth is fixed by default, one chunk of the requested size every time the pool runs dry, which
keeps the upstream traffic predictable. `pool_growth::doubling` trades that for fewer upstream calls:

```cpp
memalloc::pool_resource nodes(64, alignof(std::max_align_t), 256, memalloc::pool_growth::doubling);
std::pmr::list<int> ids(&nodes);
```

That is also how to feed a node based container: the node size is decided by the standard library,
not by you, so give the pool a block large enough to hold it.

`release()` returns every chunk to upstream and starts over. It throws `std::logic_error` if any
block is still out, because releasing memory that somebody is still holding is not a recoverable
situation:

```cpp
pool.release();
```

Any resource shared between threads:

```cpp
#include "memalloc/synchronized_resource.hpp"

auto pool = memalloc::pool_resource::for_type<particle>(4096);
memalloc::synchronized_resource shared(&pool);

std::thread worker([&shared] {
    void* storage = shared.allocate(sizeof(particle), alignof(particle));
    shared.deallocate(storage, sizeof(particle), alignof(particle));
});
```

The wrapper protects allocation and deallocation, not the observers: read `blocks_in_use()` or
`used()` only when no other thread is allocating.

Blocks that are allocated on one thread and freed on another, without a lock:

```cpp
#include "memalloc/lock_free_pool_resource.hpp"

auto events = memalloc::lock_free_pool_resource::for_type<particle>(4096);

std::thread producer([&events] {
    void* storage = events.allocate(sizeof(particle), alignof(particle));
    queue.push(new (storage) particle{});
});
std::thread consumer([&events] {
    particle* p = queue.pop();
    p->~particle();
    events.deallocate(p, sizeof(particle), alignof(particle));
});
```

The capacity is fixed at construction and exhaustion throws `std::bad_alloc`: growing a lock-free
structure needs either a lock or a way to retire memory safely, and a shared budget that silently
grows is the same stutter as a frame arena that does. `is_lock_free()` reports whether the 64 bit head
is lock free on the target, which it is on every x86, x86-64 and ARM64 platform CI builds for. Where a
64 bit atomic needs `libatomic`, the CMake target links it on its own.

Arena, one buffer per frame:

```cpp
#include "memalloc/arena_resource.hpp"

memalloc::arena_resource frame(4 * 1024 * 1024);

while (running) {
    frame.reset();
    std::pmr::vector<draw_command> commands(&frame);
    build_frame(commands);
    submit(commands);
}
```

The arena serves any alignment up to its own and refuses a stronger one with `std::bad_alloc`, so the
default matters: `arena_resource::default_alignment` is `max(alignof(std::max_align_t), 16)`, which is
16 everywhere, including MSVC x64 where `std::max_align_t` is only 8 byte aligned. For 32 or 64 byte
SIMD types, ask for it at construction:

```cpp
memalloc::arena_resource frame(4 * 1024 * 1024, 64);
```

In a test, a contract violation can be intercepted instead of aborting the process:

```cpp
[[noreturn]] void throw_instead(const char* message) {
    throw std::runtime_error(message);
}

const auto previous = memalloc::set_violation_handler(&throw_instead);
```

The handler must not return: if it does, the library prints and aborts. The handler is stored in an
atomic, so installing it while other threads run is safe; a thread that is already reporting a
violation may still call the previous one.

## Benchmark

One fixed-size type of 32 bytes, 4096 blocks per round, 256 rounds, 9 repetitions, and every
allocation is written to, so the numbers include the cost of touching the memory. Environment:
g++ 13.3.0, `-O3`, Intel Xeon at 2.10 GHz, Linux. Reproduce with `bench_alloc` before quoting any of
this.

Allocate, touch and free the same object, nanoseconds per allocate and free pair:

| Resource                              | median | best  | worst | Versus new |
|---------------------------------------|--------|-------|-------|------------|
| `new` / `delete`                      | 16.54  | 16.27 | 16.80 | 1.00x      |
| `std::pmr::unsynchronized_pool`       | 20.71  | 20.30 | 25.36 | 0.80x      |
| `memalloc::pool_resource`             | 3.75   | 3.70  | 3.89  | 4.41x      |
| `memalloc::pool` + `synchronized`     | 12.13  | 11.99 | 12.30 | 1.36x      |
| `std::vector` + free index list       | 1.56   | 1.50  | 1.76  | 10.58x     |

Allocate for one frame, release everything in one shot, nanoseconds per allocation. The baseline is
the same `new` and `delete` loop, which has to pay for every individual free:

| Resource                              | median | best  | worst | Versus new |
|---------------------------------------|--------|-------|-------|------------|
| `new` / `delete`                      | 16.54  | 16.27 | 16.80 | 1.00x      |
| `std::pmr::monotonic_buffer`          | 2.29   | 2.19  | 2.92  | 7.23x      |
| `memalloc::arena_resource`            | 1.57   | 1.54  | 1.95  | 10.55x     |

Traversal of 262144 live objects after half of them have been freed and reallocated in random order,
nanoseconds per element. This is the cost the caller pays, after the allocator has done its job:

| Layout                                | ns per element |
|---------------------------------------|----------------|
| `new` / `delete`                      | 3.29           |
| `memalloc::pool_resource`             | 2.78           |
| `std::vector` + free index list       | 1.58           |

The next two tables were measured on a different machine from the ones above: MSVC 19.51, `/O2`,
Intel Core i7-1255U, Windows 11, so compare the rows with each other, not with the Linux numbers.

Four threads sharing one resource, each allocating, touching and freeing 256 blocks per round, 512
rounds. Wall time divided by every pair of every thread:

| Resource                              | median | best  | worst | Versus new |
|---------------------------------------|--------|-------|-------|------------|
| `new` / `delete`                      | 21.83  | 21.18 | 26.14 | 1.00x      |
| `std::pmr::synchronized_pool`         | 56.30  | 50.75 | 58.25 | 0.39x      |
| `memalloc::pool` + `synchronized`     | 65.01  | 62.11 | 69.93 | 0.34x      |
| `memalloc::lock_free_pool_resource`   | 6.12   | 5.90  | 6.44  | 3.57x      |
| `memalloc::pool_resource` per thread  | 1.30   | 1.24  | 1.58  | 16.85x     |

Four threads where every block is freed by a thread other than the one that allocated it: each
thread allocates a block, swaps it into a shared ring of 1024 slots and frees whatever it took out.
The pool has exactly as many blocks as can ever be live, so it runs at full capacity throughout:

| Resource                              | median | best  | worst | Versus new |
|---------------------------------------|--------|-------|-------|------------|
| `new` / `delete`                      | 48.33  | 45.95 | 48.98 | 1.00x      |
| `std::pmr::synchronized_pool`         | 80.35  | 72.25 | 83.34 | 0.60x      |
| `memalloc::pool` + `synchronized`     | 74.36  | 70.57 | 76.55 | 0.65x      |
| `memalloc::lock_free_pool_resource`   | 16.10  | 14.83 | 16.67 | 3.00x      |

On the same machine with one thread, a pair costs 4.57 ns on `pool_resource`, 18.36 ns on
`lock_free_pool_resource` and 31.95 ns on `pool_resource` behind `synchronized_resource`.

### Reading the tables honestly

- The pool is 4.41 times faster than `new` and `delete` on allocation, and 1.18 times faster on
  traversal afterwards. Both numbers matter, and the second one is the one people forget to measure.
- A plain `std::vector` with a free index list beats the pool on both counts, by 2.4x on allocation
  and 1.76x on traversal. If your objects can be addressed by index, that is the thing to write, and
  no allocator will beat it. The pool exists for the case where you need stable pointers, an
  interface that speaks `memory_resource`, or objects that outlive the container that made them.
- The lock is not free: 12.13 - 3.75 = 8.38 ns per pair, more than twice the allocation it protects.
  Sharing one resource between threads is a decision to take on purpose.
- With the memory actually written, the arena is 2.29 / 1.57 = 1.46 times faster than
  `monotonic_buffer_resource`, not the 3.7 times it looks like when the allocation is measured in
  isolation and the pages are never touched. The write dominates.
- Shared between four threads, the sharded lock-free pool is 3.6 times faster than `new` and nine
  to ten times faster than the locked pools. With a single shared head, its first version took 40 ns,
  nearly twice as long as `new`: the shards, not the absence of a lock, are what pays.
- When every block changes thread, it is still three times faster than `new`. A similar workload on
  eight threads took between 150 and 190 ns per operation with the single head, and 7 to 12 with the
  shards.
- A pool per thread remains the fastest arrangement, 1.30 ns, because it needs no atomic at all. Use
  it when blocks stay on their thread, and the lock-free pool when they do not.
- Uncontended, the lock-free pool pays one compare and swap per call, 18.36 against 4.57 ns, and still
  costs little more than half of the mutex wrapper.

## Limitations

- `pool_resource` and `arena_resource` are not thread safe on their own. Wrap them in
  `synchronized_resource` when they must be shared, and read the accessors only while no other thread
  is allocating.
- `synchronized_resource` uses one mutex for the whole resource, so it serialises every thread. It
  buys correctness, not scalability: for a hot shared path, one resource per thread remains faster.
- `lock_free_pool_resource` has a fixed capacity of at most `max_capacity` blocks, decided at
  construction, and no `release()`. Its `blocks_in_use()` walks every shard, so it costs one step per
  free block and is exact only while no other thread is allocating or freeing.
- The shards make `lock_free_pool_resource` about 1 KB larger than the other resources, one cache line
  per shard, and the order in which blocks come back is last in, first out per shard, not across the
  whole pool. The shard count is decided at construction from `std::thread::hardware_concurrency()`
  and the capacity, with at least `min_blocks_per_shard` blocks each; `shard_count()` reports it.
- The ABA tag of `lock_free_pool_resource` is finite. A thread could be fooled only if it stalls
  between reading the head and its compare and swap while the others complete an exact multiple of
  2^tag operations and leave the same block on top. `aba_tag_bits()` reports the width: with 4096
  blocks that is 2^51 operations, years of uninterrupted churn, and never fewer than 2^32.
- `arena_resource::reset()` invalidates every pointer and runs no destructor. Destroy the objects, or
  put only trivially destructible things in the arena.
- The pool serves one block shape. `std::pmr::vector` reallocates to larger and larger sizes, so it
  throws on the pool: give vectors the arena instead.
- The contract checks are off under `NDEBUG`, and the slow ones are off unless asked for. A release
  build will not catch a double free for you.

## How to build and test

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/bench_alloc
```

The tests run clean under the sanitizers, address and undefined behaviour for all of them, thread for
the concurrent one:

```shell
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-san && ctest --test-dir build-san --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

And with the expensive contract checks turned on:

```shell
cmake -S . -B build-slow -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-DMEMALLOC_DEBUG_SLOW_CHECKS=1"
cmake --build build-slow && ctest --test-dir build-slow --output-on-failure
```

CI runs all of the above on gcc and clang, in C++17 and C++20, plus a debug build with the contract
checks on, a 32 bit build, Apple clang on ARM64, MSVC on Windows in debug and release with warnings as
errors, a benchmark smoke run, a formatting check and an install-and-consume job. See `.github/workflows/ci.yml`.

To use the library from another CMake project, either vendor it:

```cmake
add_subdirectory(external/memory-allocator)
target_link_libraries(my_game PRIVATE memalloc)
```

or install it once and find it:

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --install build --prefix /your/prefix
```

```cmake
find_package(memalloc 0.4 REQUIRED)
target_link_libraries(my_game PRIVATE memalloc::memalloc)
```

`examples/consumer` is that second flow, built and run by CI on every push.

When memalloc is consumed with `add_subdirectory`, its tests, benchmarks and install rules turn
themselves off; force them with `-DMEMALLOC_BUILD_TESTS=ON`, `-DMEMALLOC_BUILD_BENCHMARKS=ON` and
`-DMEMALLOC_INSTALL=ON`.

## Repository layout

```
include/memalloc/     the library, header only
tests/                CTest executables, no third-party dependencies
bench/                benchmark against new / delete, std::pmr and a plain vector
examples/consumer/    minimal project that consumes the installed package
cmake/                package config template for find_package
```

Contributions are welcome, `CONTRIBUTING.md` has the rules CI enforces, `CHANGELOG.md` has what
changed and why.
