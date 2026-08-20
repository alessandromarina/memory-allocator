# Changelog

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
