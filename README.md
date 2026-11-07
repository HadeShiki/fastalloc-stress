# fastalloc-stress

Stress test and lock-free extension for [fastalloc](https://github.com/HadeShiki/fastalloc)

This repo takes the original thread-local pool allocator and adds a safe cross-thread free path (`fa_mt`), along with a suite of tests to prove it doesn't corrupt memory or leak under load.

## What's inside

- `fastalloc.hpp` — The full allocator. Includes `fa::` (single-thread, fastest) and `fa_mt::` (lock-free cross-thread free).
- `stress.cpp` — Stress test runner
- `sanity.cpp` — Quick sanity checks
- `Makefile` — Cross-platform build (GCC, Clang, MSVC)

## How `fa_mt` works

The original `fa::` is strictly thread-local. Freeing a block from a different thread than the one that allocated it is undefined behavior.

`fa_mt` fixes this by keeping the fast, bump-pointer allocation path intact, but adding a lock-free Treiber stack (inbox) to every arena. When you free a block from the wrong thread, it gets pushed to the owning arena's inbox. The owner drains its inbox on its next allocation.

No mutexes. No locks on the hot path.

## Build & Run

```bash
make
make run

# Run specific modes
./build/stress_rel.exe xthread
./build/stress_rel.exe canary
./build/stress_rel.exe single
```

### Windows users: Run from a terminal with g++ or cl in your PATH (MSYS2 or Developer Command Prompt).

## Test modes
- single — 1M alloc/free on one thread. Baseline speed.
- xthread — Allocate on main, free on worker. Tests the inbox logic.
- mt — 4 producers, 4 consumers, mixed sizes. Tests heavy contention and cross-thread routing.
- canary — Fills payloads with a magic byte, verifies on free. Catches buffer overflows/underflows.

### License
License See [License](License) for details.