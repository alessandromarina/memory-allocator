# Changelog

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
