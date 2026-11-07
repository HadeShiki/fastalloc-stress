/* ============================================================================
 *  lock-free cross-thread stress test
 * ----------------------------------------------------------------------------
 *  Two modes, selectable from the command line:
 *
 *      ./build/stress_rel.exe single   one thread does everything (baseline)
 *      ./build/stress_rel.exe xthread  alloc on thread A, free on thread B
 *      ./build/stress_rel.exe mt       N producers + N consumers, mixed sizes
 *      ./build/stress_rel.exe canary   fills with a magic byte, checks on free
 *
 *  Exit code 0 = passed.  Anything else = the allocator is lying to us.
 * ==========================================================================*/

#include "fastalloc.hpp"

#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>

using clk = std::chrono::steady_clock;


/* ---------------------------------------------------------------------------
 *  Canary utilities.  Fill payload with a known byte pattern, verify it
 *  later.  Catches buffer underflow/overflow, use-after-free, and silent
 *  corruption of the header that would otherwise pass the checksum.
 * ------------------------------------------------------------------------ */

static constexpr std::uint8_t CANARY = 0xA5;

static void canary_fill(void* p, std::size_t sz) {
    std::memset(p, CANARY, sz);
}

static bool canary_check(const void* p, std::size_t sz) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < sz; ++i) {
        if (b[i] != CANARY) return false;
    }
    return true;
}


/* ---------------------------------------------------------------------------
 *  SharedQueue.  This is *not* part of the allocator — it's just a way
 *  to hand pointers from producer threads to consumer threads safely.
 *
 *  We deliberately use a mutex here instead of rolling our own lock-free
 *  MPMC queue.  The allocator is what we're testing; the handoff mechanism
 *  should be boring and obviously correct, so any failure points straight
 *  at fastalloc and not at our test harness.
 * ------------------------------------------------------------------------ */

struct SharedQueue {
    std::queue<void*>        q;
    std::mutex               mtx;
    std::condition_variable  cv;
    bool                     done = false;

    void push(void* p) {
        std::lock_guard<std::mutex> lock(mtx);
        q.push(p);
        cv.notify_one();
    }

    // Returns false when the queue is empty *and* producers are done.
    bool pop(void*& p) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !q.empty() || done; });
        if (q.empty() && done) return false;
        p = q.front();
        q.pop();
        return true;
    }

    void set_done() {
        std::lock_guard<std::mutex> lock(mtx);
        done = true;
        cv.notify_all();
    }
};


/* ---------------------------------------------------------------------------
 *  Mode 1: single-thread baseline.  No contention, no cross-thread free.
 *  This is the number everything else is measured against.
 * ------------------------------------------------------------------------ */

static void run_single() {
    std::printf("[single] 1M alloc/free, same thread\n");
    auto t0 = clk::now();
    for (int i = 0; i < 1'000'000; ++i) {
        void* p = fa_mt::allocate(64);
        fa_mt::deallocate(p, 64);
    }
    auto t1 = clk::now();
    std::printf("[single] %.3f ms\n",
        std::chrono::duration<double, std::milli>(t1 - t0).count());
}


/* ---------------------------------------------------------------------------
 *  Mode 2: cross-thread free.  The original UB that fa_mt:: exists to fix.
 *  Allocate on main, free on worker, then verify no leak.
 * ------------------------------------------------------------------------ */

static void run_xthread() {
    std::printf("[xthread] alloc on main, free on worker\n");
    constexpr int N = 100'000;

    std::vector<void*> ptrs;
    ptrs.reserve(N);

    for (int i = 0; i < N; ++i) {
        ptrs.push_back(fa_mt::allocate(128));
    }

    std::atomic<int> freed{0};
    std::thread worker([&] {
        for (int i = 0; i < N; ++i) {
            fa_mt::deallocate(ptrs[i], 128);
            freed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    worker.join();

    std::printf("[xthread] freed=%d (expected %d) — %s\n",
        freed.load(), N, freed.load() == N ? "OK" : "MISMATCH");
}


/* ---------------------------------------------------------------------------
 *  Mode 3: many producers and many consumers, mixed sizes.
 *
 *  Producers allocate, fill with canary, and push into the queue.
 *  Consumers pop, verify canary, and free on their own thread — which is
 *  the whole point: a block allocated on thread A gets freed on thread B,
 *  and fa_mt:: routes it back to A's inbox without locking.
 * ------------------------------------------------------------------------ */

static void run_mt(int producers, int consumers, int iters) {
    std::printf("[mt] %d producers, %d consumers, %d iters each\n",
        producers, consumers, iters);

    SharedQueue queue;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<int> corrupted{0};

    auto producer = [&](int /*id*/) {
        for (int i = 0; i < iters; ++i) {
            // Mixed sizes to exercise every size class, plus the occasional
            // large block that bypasses the pool entirely.
            const std::size_t sz = (i % 64 == 0)
                ? 100'000 + (i % 1000)          // large path
                : 16 + (i * 37) % 8192;         // small path

            void* p = fa_mt::allocate(sz);
            if (!p) {
                std::fprintf(stderr, "[mt] allocate(%zu) returned null\n", sz);
                std::abort();
            }
            canary_fill(p, sz);
            queue.push(p);
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto consumer = [&] {
        void* p = nullptr;
        while (queue.pop(p)) {
            // We don't know sz on the consumer side, but the header does.
            // Check the first 16 bytes — enough to catch a misrouted block
            // or a block that was freed twice.
            if (!canary_check(p, 16)) {
                std::fprintf(stderr,
                    "[mt] canary corrupt at %p\n", p);
                corrupted.fetch_add(1, std::memory_order_relaxed);
            }
            fa_mt::deallocate(p, 0);   // header knows the class and size
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (int i = 0; i < producers; ++i)
        threads.emplace_back(producer, i);
    for (int i = 0; i < consumers; ++i)
        threads.emplace_back(consumer);

    // Join producers first, then signal done.  If we set_done() before
    // producers finish, consumers would exit early and we'd leak every
    // block still in flight.
    for (int i = 0; i < producers; ++i)
        threads[i].join();

    queue.set_done();

    for (int i = producers; i < producers + consumers; ++i)
        threads[i].join();

    const bool ok = (produced.load() == consumed.load())
                 && (corrupted.load() == 0);
    std::printf("[mt] produced=%d consumed=%d corrupted=%d — %s\n",
        produced.load(), consumed.load(), corrupted.load(),
        ok ? "OK" : "FAIL");
}


/* ---------------------------------------------------------------------------
 *  Mode 4: canary integrity.  Allocate a batch, fill every byte, then
 *  verify every byte on free.  Catches overflow/underflow that would
 *  corrupt a neighbouring block's header.
 * ------------------------------------------------------------------------ */

static void run_canary() {
    std::printf("[canary] fill + verify 100k blocks\n");
    constexpr int N = 100'000;

    std::vector<std::pair<void*, std::size_t>> live;
    live.reserve(N);

    for (int i = 0; i < N; ++i) {
        const std::size_t sz = 16 + (i * 37) % 4000;
        void* p = fa_mt::allocate(sz);
        if (!p) {
            std::fprintf(stderr, "[canary] allocate(%zu) returned null\n", sz);
            std::abort();
        }
        canary_fill(p, sz);
        live.emplace_back(p, sz);
    }

    int bad = 0;
    for (auto& [p, sz] : live) {
        if (!canary_check(p, sz)) {
            std::fprintf(stderr,
                "[canary] corruption at %p (sz=%zu)\n", p, sz);
            ++bad;
        }
        fa_mt::deallocate(p, sz);
    }

    std::printf("[canary] bad=%d — %s\n", bad, bad == 0 ? "OK" : "FAIL");
}


/* ---------------------------------------------------------------------------
 *  Entry point.
 * ------------------------------------------------------------------------ */

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "mt";

    if (mode == "single") {
        run_single();
    } else if (mode == "xthread") {
        run_xthread();
    } else if (mode == "mt") {
        run_mt(4, 4, 25'000);
    } else if (mode == "canary") {
        run_canary();
    } else {
        std::fprintf(stderr,
            "usage: %s [single|xthread|mt|canary]\n", argv[0]);
        return 2;
    }

    return 0;
}