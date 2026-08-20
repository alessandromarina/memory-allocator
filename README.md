# memory-allocator - fixed-size pool and frame arena for C++

Two allocators for games and other soft real-time code, exposed as `std::pmr::memory_resource`:
a pool for one fixed-size type and a linear arena reset once per frame, plus a wrapper that makes
either of them thread safe. Header only, C++17, no dependencies.

- Language: C++17, header only
- Build: CMake 3.16 or newer
- Version: 0.2.0
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
| `arena_resource`, bump pointer with reset            | 🟢     |
| `synchronized_resource`, mutex wrapper               | 🟢     |
| Fixed or doubling chunk growth                       | 🟢     |
| Argument validation on construction                  | 🟢     |
| Contract checks in debug builds, custom handler       | 🟢     |
| Randomised property test against an oracle           | 🟢     |
| Benchmark, median and spread, against three rivals   | 🟢     |
| CI on gcc, clang, MSVC, with sanitizers              | 🟢     |
| `install` and `find_package` support                 | 🟢     |
| Lock-free variant for shared use                     | 🔴     |

## Overview

Games allocate in two shapes. Many objects of the same type, created and destroyed continuously
(particles, projectiles, event nodes), and a lot of scratch data that lives exactly one frame and
then dies all at once. The general purpose allocator behind `new` has to serve every shape, so it
pays for size classes, coalescing and thread safety on every call.

`pool_resource` serves one block shape. Free blocks are threaded through a singly linked list stored
in the blocks themselves, so allocate is a pointer read and deallocate is a pointer write. Memory is
carved from chunks taken from an upstream resource, and every chunk is returned to it in the
destructor or in `release()`.

`arena_resource` owns one buffer and a cursor. Allocation aligns the cursor and moves it forward,
deallocation does nothing, and `reset()` sets the cursor back to zero. There is no per-object
bookkeeping at all, which is also why it demands the most discipline: `reset()` invalidates every
pointer handed out and calls no destructor.

Both derive from `std::pmr::memory_resource`, so they work with `std::pmr` containers and with any
code that takes a `memory_resource*`, without templates leaking into the call sites.

## Design decisions

**The lock is a decorator, not a flag.** Neither allocator takes a lock, because most of the time
there is nothing to lock: one resource per thread is the arrangement that makes them fast. When a
resource really has to be shared, `synchronized_resource` wraps any other resource and serialises
`allocate` and `deallocate` behind a mutex. Keeping the lock outside means the single threaded path
stays exactly as it was, and the price of sharing is visible in the type at the call site.

**Neither type is copyable or movable**, exactly like `std::pmr::monotonic_buffer_resource`. A
`memory_resource` hands its address to every container built on it, so an object that can be moved out
from under those containers is a dangling pointer waiting to happen. Address stability is the feature.
To keep several of them, use `std::deque<pool_resource>` with `emplace_back`, which never moves its
elements, or `std::vector<std::unique_ptr<pool_resource>>`.

**Exhaustion is an error, not a growth event.** The arena has a fixed capacity and throws
`std::bad_alloc` instead of falling back upstream. A frame budget that silently grows is a stutter
later, in a build where nobody is looking. `high_water_mark()` is there to size the budget once, from
a real run.

**Contract violations are reported, not ignored.** In debug builds the pool refuses a pointer it never
handed out, a pointer that is not on a block boundary, and one free too many; with
`MEMALLOC_DEBUG_SLOW_CHECKS=1` it also scans the free list and catches a double free while other
blocks are live. All of it compiles away under `NDEBUG`, and none of it changes the object layout, so
a debug and a release translation unit still agree on what these classes look like.

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

In a test, a contract violation can be intercepted instead of aborting the process:

```cpp
[[noreturn]] void throw_instead(const char* message) {
    throw std::runtime_error(message);
}

const auto previous = memalloc::set_violation_handler(&throw_instead);
```

The handler must not return: if it does, the library prints and aborts. Installing it is not thread
safe, do it before starting the threads.

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

## Limitations

- `pool_resource` and `arena_resource` are not thread safe on their own. Wrap them in
  `synchronized_resource` when they must be shared, and read the accessors only while no other thread
  is allocating.
- `synchronized_resource` uses one mutex for the whole resource, so it serialises every thread. It
  buys correctness, not scalability: for a hot shared path, one resource per thread remains faster.
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

CI runs all of the above on gcc and clang, in C++17 and C++20, plus MSVC on Windows, plus a
formatting check and an install-and-consume job. See `.github/workflows/ci.yml`.

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
find_package(memalloc 0.2 REQUIRED)
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

Contributions are welcome, `CONTRIBUTING.md` has the rules CI enforces.
