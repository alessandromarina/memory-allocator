# Contributing

Pull requests are welcome. The rules below are what CI enforces, so a change that follows them is a
change that merges.

## Before opening a pull request

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror"
cmake --build build
ctest --test-dir build --output-on-failure

clang-format --dry-run --Werror $(git ls-files '*.hpp' '*.cpp')
```

Then at least one sanitizer run, all of them if you touched the allocators:

```shell
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-san && ctest --test-dir build-san --output-on-failure
```

## What a change is expected to carry

- A test that fails before the change and passes after it. Bug fixes without a reproducing test are
  not merged, because nothing stops the bug from coming back.
- No new dependency. The library is header only and the test and benchmark harnesses are hand written
  on purpose: cloning and building must never need the network.
- Formatting from `.clang-format`, not from taste. Run the formatter, do not reformat code you did
  not touch.
- No comments explaining what the code does. If a function needs a comment to be understood, rename
  it or split it. Documentation for users lives in `README.md`.

## Performance changes

A pull request that claims a speed-up carries `bench_alloc` output from before and after, on the same
machine, in the description. Median and spread, not a single best-case number. A change that is
faster on one workload and slower on another is fine, as long as the trade-off is stated.

## Public API changes

The two allocators are deliberately narrow: one block shape for the pool, one buffer for the arena.
New parameters, new modes and new allocators need a use case that the existing ones cannot serve.
Open an issue first, so the discussion happens before the code.
