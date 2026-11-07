/* ============================================================================
 *  fastalloc.hpp — thread-local pool allocator for C++17  (+ MT extension)
 * ----------------------------------------------------------------------------
 *  by HadeShiki  <https://github.com/HadeShiki>
 *
 *  Drop this file in, #include it, and use fa::Allocator<T> with your STL
 *  containers.  That's it.  No build step, no link flags, no dependencies.
 *
 *  HOW IT WORKS (one paragraph):
 *
 *      Each thread gets its own arena.  The arena asks the OS for big chunks
 *      (256 KiB) and hands out memory by bumping a pointer — that's one add.
 *      When you free, the block goes onto a free list for its size class.
 *      Next allocation of the same size pops it off.  No coalescing, no locks,
 *      no bookkeeping beyond what's needed to catch bugs.
 *
 *  TWO NAMESPACES:
 *
 *      fa::      single-threaded.  Fastest.  Free on the SAME thread that
 *                allocated.  Cross-thread free is undefined behavior.
 *
 *      fa_mt::   multi-threaded, lock-free.  Cross-thread free is safe —
 *                the block is routed to its owner via a Treiber stack of
 *                returned blocks.  Slightly slower on the free path when
 *                crossing threads, identical on the fast path.
 *
 *  MEMORY LAYOUT
 *  -------------
 *
 *      OS chunk (mmap / VirtualAlloc)
 *      +-------------------------------------------------------------+
 *      | Chunk hdr | pad | block | block | block | ... free space ...|
 *      +-------------------------------------------------------------+
 *                 ^
 *                 bump_ points here, moves right on each alloc
 *
 *
 *      One allocation, zoomed in:
 *
 *          user pointer
 *          |
 *          v
 *      +---------+---------+--------------------------------+
 *      | header  | payload (what you asked for)             |
 *      | 16 B    | size class rounds up, so may be larger   |
 *      +---------+--------------------------------+
 *      ^
 *      |__ BlockHeader { magic, class, owner_id, checksum, pad }
 *
 *  THINGS YOU SHOULD KNOW
 *  ----------------------
 *    * fa:: requires same-thread free.  fa_mt:: does not.
 *    * Memory does not go back to the OS until the thread exits (fa::) or
 *      the owning arena is destroyed (fa_mt::).
 *    * This is NOT operator new replacement.  Do not override global new.
 *
 *  BUILD
 *  -----
 *      g++     -std=c++17 -O2 test.cpp -o test
 *      clang++ -std=c++17 -O2 test.cpp -o test
 *      cl      /std:c++17 /O2 test.cpp
 *
 *  LICENSE: public domain / CC0.  Do whatever you want with it.
 * ==========================================================================*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <atomic>
#include <cassert>
#include <utility>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif


namespace fa {

/* ---------------------------------------------------------------------------
 *  Tunables.  Change these if you know what you're doing.
 * ------------------------------------------------------------------------ */

// How much we ask the OS for at a time.  Bigger = fewer syscalls but more
// wasted tail space if the thread only does a handful of allocations.
constexpr std::size_t CHUNK_SIZE = 256u * 1024u;

// Anything above this bypasses the pool entirely.  Big blocks are rare and
// pooling them wastes chunk space (they'd gobble a whole chunk).
constexpr std::size_t LARGE_THRESHOLD = 64u * 1024u;

// Size classes.  Must be strictly increasing, each a multiple of 16 so the
// payload is always 16-byte aligned without per-block padding.
constexpr std::size_t NUM_CLASSES = 10;
constexpr std::size_t CLASS_SIZES[NUM_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
};

// Largest block (user size + header) that still fits a size class.
constexpr std::size_t MAX_SMALL_BLOCK = CLASS_SIZES[NUM_CLASSES - 1];


/* ---------------------------------------------------------------------------
 *  OS glue.  We want page-aligned zeroed memory.
 * ------------------------------------------------------------------------ */

inline void* os_alloc(std::size_t sz) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

inline void os_free(void* p, std::size_t sz) {
    if (!p) return;
#if defined(_WIN32)
    (void)sz;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, sz);
#endif
}


/* ---------------------------------------------------------------------------
 *  Size class lookup.  Branchy on purpose.
 * ------------------------------------------------------------------------ */

inline int size_to_class(std::size_t block) {
    if (block <= 16)    return 0;
    if (block <= 32)    return 1;
    if (block <= 64)    return 2;
    if (block <= 128)   return 3;
    if (block <= 256)   return 4;
    if (block <= 512)   return 5;
    if (block <= 1024)  return 6;
    if (block <= 2048)  return 7;
    if (block <= 4096)  return 8;
    if (block <= 8192)  return 9;
    return -1;
}


/* ---------------------------------------------------------------------------
 *  Header sitting just before every user pointer.
 *
 *      offset  size  field
 *      ------  ----  -----------------------------------------
 *        0      4    magic       (0xFAFAFAFA live, 0xDEADBEEF freed)
 *        4      2    size_class  (0xFFFF for large blocks)
 *        6      2    owner_id    (which arena owns this — used by fa_mt)
 *        8      4    checksum    (XOR of everything above)
 *       12      4    pad         (keeps payload 16-aligned)
 *
 *  For fa::, owner_id is always 0 and ignored.  For fa_mt::, it indexes
 *  the arena registry so a cross-thread free knows where to deliver the
 *  block.
 * ------------------------------------------------------------------------ */

struct alignas(16) BlockHeader {
    std::uint32_t magic;
    std::uint16_t size_class;
    std::uint16_t owner_id;
    std::uint32_t checksum;
    std::uint32_t pad;
};

constexpr std::uint32_t MAGIC_LIVE = 0xFAFAFAFAu;
constexpr std::uint32_t MAGIC_FREE = 0xDEADBEEFu;
constexpr std::uint16_t CLASS_LARGE = 0xFFFFu;

inline std::uint32_t header_checksum(const BlockHeader& h) {
    return h.magic
         ^ (static_cast<std::uint32_t>(h.size_class) << 16)
         ^ h.owner_id
         ^ h.pad;
}


/* ---------------------------------------------------------------------------
 *  Free list node.  Overlaps the user payload of a freed block.
 * ------------------------------------------------------------------------ */

struct FreeNode { FreeNode* next; };


/* ---------------------------------------------------------------------------
 *  Arena.  One per thread.  Owns its chunks; never shares with anyone.
 *  Works exclusively in *block* sizes (user + header).
 * ------------------------------------------------------------------------ */

class Arena {
public:
    Arena() noexcept
        : head_(nullptr), bump_(nullptr), bump_end_(nullptr) {
        for (auto& fl : free_lists_) fl = nullptr;
    }

    ~Arena() {
        Chunk* c = head_;
        while (c) {
            Chunk* next = c->next;
            os_free(c, c->size);
            c = next;
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(std::size_t block) noexcept {
        if (block == 0) block = 1;

        if (block > LARGE_THRESHOLD)
            return large_alloc(block);

        const int cls = size_to_class(block);
        if (cls < 0)
            return large_alloc(block);

        if (FreeNode* n = free_lists_[cls]) {
            free_lists_[cls] = n->next;
            return n;
        }

        const std::size_t need = CLASS_SIZES[cls];
        if (!bump_ || bump_ + need > bump_end_) {
            if (!refill(need)) return nullptr;
        }
        void* p = bump_;
        bump_ += need;
        return p;
    }

    void deallocate(void* p, std::size_t block) noexcept {
        if (!p) return;

        if (block > LARGE_THRESHOLD) {
            large_free(p);
            return;
        }

        const int cls = size_to_class(block);
        if (cls < 0) {
            large_free(p);
            return;
        }

        FreeNode* n = static_cast<FreeNode*>(p);
        n->next = free_lists_[cls];
        free_lists_[cls] = n;
    }

private:
    struct Chunk {
        Chunk*      next;
        std::size_t size;
        std::size_t pad;
    };

    Chunk*    head_;
    char*     bump_;
    char*     bump_end_;
    FreeNode* free_lists_[NUM_CLASSES];

    static constexpr std::size_t HEADER_PAD =
        (sizeof(Chunk) + 15) & ~std::size_t(15);

    bool refill(std::size_t need) noexcept {
        std::size_t chunk_sz = CHUNK_SIZE;
        if (chunk_sz < HEADER_PAD + need)
            chunk_sz = HEADER_PAD + need;

        const std::size_t page = 4096;
        chunk_sz = (chunk_sz + page - 1) & ~(page - 1);

        Chunk* c = static_cast<Chunk*>(os_alloc(chunk_sz));
        if (!c) return false;

        c->size = chunk_sz;
        c->next = head_;
        head_   = c;

        bump_     = reinterpret_cast<char*>(c) + HEADER_PAD;
        bump_end_ = reinterpret_cast<char*>(c) + chunk_sz;
        return true;
    }

    void* large_alloc(std::size_t block) noexcept {
        const std::size_t page = 4096;
        std::size_t total = (HEADER_PAD + block + page - 1) & ~(page - 1);

        Chunk* c = static_cast<Chunk*>(os_alloc(total));
        if (!c) return nullptr;
        c->size = total;
        c->next = nullptr;

        void* p = reinterpret_cast<char*>(c) + HEADER_PAD;
        assert(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
        return p;
    }

    void large_free(void* p) noexcept {
        Chunk* c = reinterpret_cast<Chunk*>(static_cast<char*>(p) - HEADER_PAD);
        os_free(c, c->size);
    }
};


/* ---------------------------------------------------------------------------
 *  thread_local arena for fa:: (single-thread API).
 * ------------------------------------------------------------------------ */

inline Arena& tls_arena() {
    thread_local Arena a;
    return a;
}


/* ---------------------------------------------------------------------------
 *  Stats.  Cheap, optional.
 * ------------------------------------------------------------------------ */

struct Stats {
    std::atomic<std::uint64_t> allocs{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> large_allocs{0};
};

inline Stats& stats() {
    static Stats s;
    return s;
}


/* ---------------------------------------------------------------------------
 *  Public API — fa:: (single-threaded)
 * ------------------------------------------------------------------------ */

inline void* allocate(std::size_t sz) noexcept {
    if (sz == 0) sz = 1;

    const std::size_t block = sz + sizeof(BlockHeader);
    void* raw = tls_arena().allocate(block);
    if (!raw) return nullptr;

    const int cls = (block > LARGE_THRESHOLD) ? -1 : size_to_class(block);

    BlockHeader* h = static_cast<BlockHeader*>(raw);
    h->magic      = MAGIC_LIVE;
    h->size_class = (cls < 0) ? CLASS_LARGE
                              : static_cast<std::uint16_t>(cls);
    h->owner_id   = 0;   // fa:: never crosses threads
    h->pad        = 0;
    h->checksum   = header_checksum(*h);

    stats().allocs.fetch_add(1, std::memory_order_relaxed);
    stats().bytes .fetch_add(sz, std::memory_order_relaxed);
    if (cls < 0)
        stats().large_allocs.fetch_add(1, std::memory_order_relaxed);

    return h + 1;
}

inline void deallocate(void* p, std::size_t sz) noexcept {
    if (!p) return;

    BlockHeader* h = static_cast<BlockHeader*>(p) - 1;

    if (h->magic != MAGIC_LIVE || h->checksum != header_checksum(*h)) {
        std::fprintf(stderr,
            "[fastalloc] bad header at %p (magic=%08x, want %08x)\n",
            p, h->magic, MAGIC_LIVE);
        std::abort();
    }

    h->magic = MAGIC_FREE;

    if (h->size_class != CLASS_LARGE) {
        const std::size_t block = CLASS_SIZES[h->size_class];
        tls_arena().deallocate(h, block);
    } else {
        const std::size_t block = sz + sizeof(BlockHeader);
        tls_arena().deallocate(h, block);
    }

    stats().frees.fetch_add(1, std::memory_order_relaxed);
}

inline std::size_t usable_size(void* p) noexcept {
    if (!p) return 0;
    BlockHeader* h = static_cast<BlockHeader*>(p) - 1;
    if (h->magic != MAGIC_LIVE)      return 0;
    if (h->size_class == CLASS_LARGE) return 0;
    return CLASS_SIZES[h->size_class] - sizeof(BlockHeader);
}


/* ---------------------------------------------------------------------------
 *  STL allocator (fa::)
 * ------------------------------------------------------------------------ */

template <typename T>
struct Allocator {
    using value_type = T;

    Allocator() noexcept = default;
    template <typename U>
    Allocator(const Allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* p = fa::allocate(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        fa::deallocate(p, n * sizeof(T));
    }

    template <typename U>
    bool operator==(const Allocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const Allocator<U>&) const noexcept { return false; }
};


/* ---------------------------------------------------------------------------
 *  RAII helpers
 * ------------------------------------------------------------------------ */

template <typename T, typename... Args>
T* make(Args&&... args) {
    void* p = allocate(sizeof(T));
    if (!p) throw std::bad_alloc();
    return new (p) T(std::forward<Args>(args)...);
}

template <typename T>
void destroy(T* p) noexcept {
    if (!p) return;
    p->~T();
    deallocate(p, sizeof(T));
}

}


/* ============================================================================
 *  multi-threaded, lock-free extension
 * ----------------------------------------------------------------------------
 *  Keeps per-thread arena for *allocation* (still one add).  For free,
 *  routes the block to the owning arena via a lock-free Treiber stack of
 *  "returned blocks".  The owner drains its inbox on its next allocation.
 *
 *  No mutex.  No syscall on the free path.  Cache-friendly.
 * ==========================================================================*/

namespace fa_mt {

constexpr std::size_t MAX_ARENAS = 64;   // tune to your machine


/* ---------- per-arena inbox: lock-free Treiber stack -------------------- */

struct InboxNode { InboxNode* next; };

struct alignas(64) Inbox {
    std::atomic<InboxNode*> head{nullptr};

    void push(InboxNode* n) noexcept {
        InboxNode* old = head.load(std::memory_order_relaxed);
        do {
            n->next = old;
        } while (!head.compare_exchange_weak(
            old, n, std::memory_order_release, std::memory_order_relaxed));
    }

    InboxNode* pop() noexcept {
        InboxNode* old = head.load(std::memory_order_acquire);
        while (old) {
            InboxNode* next = old->next;
            if (head.compare_exchange_weak(
                    old, next, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return old;
        }
        return nullptr;
    }
};


/* ---------- arena registry: fixed, lock-free after init ---------- */

struct alignas(64) ArenaEntry {
    fa::Arena     arena;
    Inbox         inbox;
    std::uint16_t id = 0;
};

inline ArenaEntry* arena_table() noexcept {
    static ArenaEntry t[MAX_ARENAS];
    return t;
}

inline std::atomic<std::uint16_t>& next_id() noexcept {
    static std::atomic<std::uint16_t> n{0};
    return n;
}

inline ArenaEntry& thread_arena() {
    thread_local ArenaEntry* entry = nullptr;
    if (!entry) {
        const unsigned idx = next_id().fetch_add(1);
        if (idx >= MAX_ARENAS) {
            std::fprintf(stderr,
                "[fa_mt] too many threads (%u > %zu). Raise MAX_ARENAS.\n",
                idx, MAX_ARENAS);
            std::abort();
        }
        entry = &arena_table()[idx];
        entry->id = static_cast<std::uint16_t>(idx);
    }
    return *entry;
}


/* ---------- public API — fa_mt:: (lock-free cross-thread-safe) ---------- */

inline void* allocate(std::size_t sz) noexcept {
    if (sz == 0) sz = 1;
    ArenaEntry& me = thread_arena();

    // Drain my inbox: these are blocks other threads returned to me.
    // Draining before bumping keeps the local free list warm.
    while (InboxNode* n = me.inbox.pop()) {
        fa::BlockHeader* h =
            reinterpret_cast<fa::BlockHeader*>(n) - 1;
        // We flipped magic to FREE on the freeing thread, but we must
        // restore LIVE before putting it back on the free list, otherwise
        // the *next* allocate that reuses this block will trip the
        // checksum on a subsequent free.
        h->magic    = fa::MAGIC_LIVE;
        h->checksum = fa::header_checksum(*h);

        const std::size_t block = (h->size_class == fa::CLASS_LARGE)
            ? fa::LARGE_THRESHOLD + 1      // large_free path
            : fa::CLASS_SIZES[h->size_class];
        me.arena.deallocate(h, block);
    }

    const std::size_t block = sz + sizeof(fa::BlockHeader);
    void* raw = me.arena.allocate(block);
    if (!raw) return nullptr;

    const int cls = (block > fa::LARGE_THRESHOLD)
        ? -1 : fa::size_to_class(block);

    fa::BlockHeader* h = static_cast<fa::BlockHeader*>(raw);
    h->magic      = fa::MAGIC_LIVE;
    h->size_class = (cls < 0) ? fa::CLASS_LARGE
                              : static_cast<std::uint16_t>(cls);
    h->owner_id   = me.id;
    h->pad        = 0;
    h->checksum   = fa::header_checksum(*h);

    fa::stats().allocs.fetch_add(1, std::memory_order_relaxed);
    fa::stats().bytes .fetch_add(sz, std::memory_order_relaxed);
    if (cls < 0)
        fa::stats().large_allocs.fetch_add(1, std::memory_order_relaxed);

    return h + 1;
}

inline void deallocate(void* p, std::size_t sz) noexcept {
    if (!p) return;

    fa::BlockHeader* h = static_cast<fa::BlockHeader*>(p) - 1;

    if (h->magic != fa::MAGIC_LIVE ||
        h->checksum != fa::header_checksum(*h)) {
        std::fprintf(stderr,
            "[fa_mt] bad header at %p (magic=%08x)\n", p, h->magic);
        std::abort();
    }

    if (h->owner_id >= MAX_ARENAS) {
        std::fprintf(stderr,
            "[fa_mt] corrupt owner_id %u at %p\n", h->owner_id, p);
        std::abort();
    }

    h->magic = fa::MAGIC_FREE;

    ArenaEntry& owner = arena_table()[h->owner_id];

    // Same thread as owner?  Fast path: straight to free list.
    if (&owner == &thread_arena()) {
        const std::size_t block = (h->size_class == fa::CLASS_LARGE)
            ? sz + sizeof(fa::BlockHeader)
            : fa::CLASS_SIZES[h->size_class];
        owner.arena.deallocate(h, block);
    } else {
        // Cross-thread: push to owner's inbox.  Lock-free.
        // We reuse the payload as an InboxNode — safe because the block
        // is now logically dead (magic == FREE).
        auto* n = reinterpret_cast<InboxNode*>(h + 1);
        owner.inbox.push(n);
    }

    fa::stats().frees.fetch_add(1, std::memory_order_relaxed);
}

inline std::size_t usable_size(void* p) noexcept {
    return fa::usable_size(p);
}


/* ---------- STL allocator adapter (fa_mt) ---------- */

template <typename T>
struct Allocator {
    using value_type = T;

    Allocator() noexcept = default;
    template <typename U>
    Allocator(const Allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        void* p = fa_mt::allocate(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t n) noexcept {
        fa_mt::deallocate(p, n * sizeof(T));
    }
    template <typename U>
    bool operator==(const Allocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const Allocator<U>&) const noexcept { return false; }
};


/* ---------- RAII helpers (fa_mt) ---------- */

template <typename T, typename... Args>
T* make(Args&&... args) {
    void* p = allocate(sizeof(T));
    if (!p) throw std::bad_alloc();
    return new (p) T(std::forward<Args>(args)...);
}

template <typename T>
void destroy(T* p) noexcept {
    if (!p) return;
    p->~T();
    deallocate(p, sizeof(T));
}

}