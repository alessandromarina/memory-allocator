# Changelog

## 0.5.1

- `lock_free_pool_resource` moves a thread to its CPU's shard on contention only when the pool has at
  least as many shards as `std::thread::hardware_concurrency()`. In 0.5.0 a pool with fewer shards
  than CPUs folded the CPUs onto too few shards and spread the threads worse than the round robin
  slots: four threads on two shards went from 13 to 21 ns per pair on Linux and from 19 to 28 on
  Windows, eight threads on four shards from 8 to 15 and from 12 to 18. Such pools now behave as in
  0.4.0, 13 and 20 ns, 9 and 12. Pools with a shard per CPU keep the gain of 0.5.0: four colliding
  threads take 8 ns instead of 35 on Linux and 55 on Windows. `spreads_threads_by_cpu()` reports
  which case a pool is in.
- Measured on Linux with g++ 14 and clang 18: `bench_alloc` is unchanged from 0.4.0 within noise, as
  on MSVC. The README now says that its ratios against `new` are MSVC's, and gives glibc's.
- The README corrects what `hardware_concurrency()` means in a container: the host's CPU count, which
  is the right shard count under a CPU quota, not just more shards than needed.
- `test_pool_property` no longer trips the false `-Wfree-nonheap-object` of GCC 14 with `-O2 -Werror`.

## 0.5.0

- `pool_resource`, `arena_resource` and `lock_free_pool_resource` have `try_allocate`, which returns
  `nullptr` instead of throwing and is `noexcept`. On `pool_resource` it also returns `nullptr` when
  the upstream throws while the pool grows, and leaves the pool usable.
- The library builds with exceptions disabled. `allocate` that cannot succeed, a constructor given
  impossible arguments and `pool_resource::release` with live blocks call the violation handler
  instead of throwing. `test_no_exceptions` is compiled with `-fno-exceptions -fno-rtti`, or
  `/EHs-c- /GR-` on MSVC, by every CI job, and checks each of those reports in its own process.
- A thread that loses a compare and swap on `lock_free_pool_resource` moves to the shard of the CPU
  it runs on, on Windows and Linux. Four threads whose round robin slots landed on the same shard went
  from 55 to 8 to 10 ns per pair, 24 threads on 12 hardware threads from 5.4 to 3.5, with no change
  where threads do not collide. Choosing the shard from the CPU on every call was measured too, and
  was 20 to 60 percent slower when threads do not collide. `moves_threads_on_contention` reports
  whether the platform supports it.
- The compare and swap retry loops of `lock_free_pool_resource` and its steal path live out of line,
  so the uncontended allocate and free are one load and one compare and swap, inlined. On MSVC the
  hot path of `do_allocate` went from 87 to 66 instructions and saves fewer registers, and a retry
  loop that grows can no longer push the fast path over a compiler's inlining threshold.
- `test_lock_free_interleaving` explores 5544 deterministic interleavings of the lock-free pool, in
  one thread, through interleaving points that compile to nothing outside the test. It fails if the
  ABA tag or the second collect of the exhaustion check is removed; the threaded tests pass in both
  cases.
- CI runs every ctest with a timeout, and `test_no_exceptions` turns off the abort dialog of the MSVC
  debug runtime, so a failure ends the job instead of hanging it.

## 0.4.0

- `lock_free_pool_resource` splits its free list into up to 16 shards, each a Treiber stack on its own
  cache line. A thread allocates from and frees into the shard its global thread slot picks and steals
  from the others only when that shard is empty. With four threads it went from 40 to about 6 ns per
  pair, 3.6 times faster than `new`. When every block is freed by another thread it takes about 16 ns,
  three times faster than `new`; a similar eight thread workload went from 150 to 190 ns per
  operation with the single head down to 7 to 12.
- Exhaustion stays exact: when a pass finds every shard empty, a second pass checks that no shard head
  changed in between, and only then throws `std::bad_alloc`. Every push and pop advances the tag, so
  an unchanged head means the shard was empty throughout, and a block that is free is never reported
  as missing.
- The per thread slot is a constant initialised `thread_local`, so reading it costs no guard check, and
  the uncontended path is as fast as with a single head.
- Added `shard_count()`, `max_shards` and `min_blocks_per_shard`.
- The virtual overrides of every resource are `final`, so GCC and Clang can devirtualize calls made
  through the concrete type. MSVC keeps the indirect call; measured, it costs about 0.35 ns per pair.
- The benchmark gained a section where every block is freed by another thread.

## 0.3.1

- The ABA tag of `lock_free_pool_resource` takes every bit of the 64 bit head that the block index
  does not need, instead of a fixed 32: 51 bits for 4096 blocks, 43 for a million, never fewer than
  32. The hot path is unchanged, a mask and an add. `aba_tag_bits()` reports the width.
- `pool_resource` and `lock_free_pool_resource` report, in debug builds, a `deallocate` whose size or
  alignment the pool could not have served.
- The violation handler is a `std::atomic`, so `set_violation_handler` is safe while other threads
  run.
- CMake probes whether 64 bit atomics link on their own and links `libatomic` to the target where
  they do not, so `lock_free_pool_resource` links on targets without native 64 bit atomics.
- The package version file uses `SameMinorVersion`: in 0.x a minor release may break the interface,
  so `find_package(memalloc 0.3)` no longer accepts a future 0.4.
- CI uses `actions/checkout@v7`, which runs on the Node 24 LTS runtime instead of the deprecated
  Node 20.

## 0.3.0

- Added `lock_free_pool_resource`, a fixed capacity pool that several threads can allocate from and
  free into at the same time without a lock. The free list is a Treiber stack with a 32 bit index and
  a 32 bit ABA tag packed in one 64 bit atomic, and the links live outside the blocks, so the type has
  no data race and runs clean under ThreadSanitizer. Under contention on four threads it is about 1.75
  times faster than `pool_resource` behind `synchronized_resource`. Failed compare and swap attempts
  back off exponentially and the hot path keeps no shared counter, `blocks_in_use()` walks the free
  list instead: together they took its contended cost from 145 to 40 ns per pair during development.
  In debug builds it catches a double free in constant time.
- `pool_resource` carves a new chunk lazily, one block per allocation, instead of threading the whole
  chunk onto the free list when it is added. The allocation that triggers growth no longer touches
  every page of the chunk: on a 65536 block chunk it went from about 630 to about 3 microseconds.
  A pointer into the uncarved tail of a chunk is now refused by the debug checks.
- `pool_resource::owns()` and the debug checks find the owning chunk with a binary search over chunks
  kept in address order, instead of a linear scan.
- `arena_resource` no longer updates its high water mark on every allocation; it is folded in on
  `reset()` and when it is read, with the same result.
- The benchmark gained the lock-free pool and a four thread contended section against `new`,
  `std::pmr::synchronized_pool_resource`, the locked pool and one pool per thread.
- CI gained a debug build with the contract checks on, clang in C++20, Apple clang on ARM64, MSVC in
  debug as well as release with warnings as errors, a benchmark smoke run, and cancels superseded runs.

## 0.2.1

- `arena_resource::default_alignment` is now `max(alignof(std::max_align_t), 16)` instead of
  `alignof(std::max_align_t)` alone. On MSVC x64 that value is 8, not 16 as on Linux x86-64, so an
  arena built with the default refused a 16 byte aligned allocation and the uncaught `std::bad_alloc`
  ended the process through the CRT with `0xC0000409`. The default now covers 16 byte SIMD types on
  every platform, and two `static_assert` pin the invariant.
- The test harness reports an unhandled exception with its message and exits with 2, instead of
  letting the runtime abort with a platform specific code.
- CI gained a 32 bit build, which exercises the size arithmetic and the overflow checks with a 32 bit
  `std::size_t`.

## 0.2.0

- Added `synchronized_resource`, a mutex decorator that makes any resource thread safe without
  putting a lock in the hot single threaded path.
- Added `pool_growth::doubling`, `pool_resource::release()` and `owns()` on both allocators.
- Constructor arguments are validated and rejected with `std::invalid_argument`, including the size
  overflows that used to wrap silently.
- Added contract checks for the pool and the arena, off under `NDEBUG`, with a replaceable violation
  handler, plus the expensive double free detection behind `MEMALLOC_DEBUG_SLOW_CHECKS`.
- Added a randomised property test against an oracle, and rewrote the benchmark: the allocated memory
  is now written to, results report median and spread, and two rivals were added, a locked pool and a
  plain `std::vector` with a free index list, plus a traversal benchmark after fragmentation.
- Added `install` and `find_package` support, a consumer example, `.clang-format`, and CI on gcc,
  clang and MSVC with sanitizers.

## 0.1.0

- First version: `pool_resource`, `arena_resource`, tests and a benchmark against `new` and
  `std::pmr`.
