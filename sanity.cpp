#include "fastalloc.hpp"
#include <vector>
#include <thread>
#include <cstdio>

int main() {
    // 1. fa:: — same-thread
    {
        std::vector<int, fa::Allocator<int>> v;
        for (int i = 0; i < 1000; ++i) v.push_back(i);
        std::printf("fa::  v.size=%zu\n", v.size());
    }

    // 2. fa_mt:: — cross-thread free
    {
        void* p = fa_mt::allocate(128);
        std::thread t([p]{ fa_mt::deallocate(p, 128); });
        t.join();
        std::printf("fa_mt:: cross-thread OK\n");
    }

    // 3. fa_mt:: — reuse after drain (must not abort)
    {
        void* p = fa_mt::allocate(64);
        std::thread t([p]{ fa_mt::deallocate(p, 64); });
        t.join();
        for (int i = 0; i < 100; ++i) {
            void* q = fa_mt::allocate(64);   // triggers drain
            fa_mt::deallocate(q, 64);
        }
        std::printf("fa_mt:: drain + reuse OK\n");
    }
    return 0;
}