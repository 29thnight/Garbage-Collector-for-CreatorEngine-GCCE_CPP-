// The block allocator alone against global operator new/delete, with the
// block sizes the GC actually requests (object header + object).
//
//     gcce_allocator_bench [scale]

#include "block_allocator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

namespace
{
using clock_type = std::chrono::steady_clock;

double ns_per(clock_type::time_point start, std::size_t ops)
{
    return std::chrono::duration<double, std::nano>(clock_type::now() - start).count() / static_cast<double>(ops);
}

// Header (~72 bytes) plus the payload mix of the GC benchmark.
constexpr std::array<std::size_t, 6> sizes{104, 136, 200, 328, 584, 1096};
constexpr std::array<int, 6> weights{40, 25, 15, 10, 7, 3};

struct block
{
    void* p;
    std::size_t size;
    std::uint16_t pool;
};

// One type key per block size stands in for one type per size.
template <gc::detail::pool_mode Mode>
struct pool_heap
{
    gc::detail::block_allocator a{Mode};
    block alloc(std::size_t size)
    {
        auto r = a.allocate(size, 8, reinterpret_cast<const void*>(size));
        return {r.block, size, r.pool};
    }
    void free(const block& b) { a.deallocate(b.p, b.size, 8, b.pool); }
};

struct system_heap
{
    block alloc(std::size_t size) { return {::operator new(size), size, 0}; }
    void free(const block& b) { ::operator delete(b.p); }
};

template <class Heap>
void run(const char* name, std::size_t n, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    std::vector<std::size_t> plan(n);
    for (auto& s : plan)
        s = sizes[static_cast<std::size_t>(dist(rng))];
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = i;
    std::shuffle(order.begin(), order.end(), rng);

    Heap heap;
    std::vector<block> blocks(n);
    double fill = 0;
    double random_free = 0;
    double refill = 0;
    double free_all = 0;
    for (int rep = 0; rep < 5; ++rep)
    {
        auto t = clock_type::now();
        for (std::size_t i = 0; i < n; ++i)
            blocks[i] = heap.alloc(plan[i]);
        fill += ns_per(t, n);

        // Free half in random order, as a sweep over a fragmented heap does.
        t = clock_type::now();
        for (std::size_t i = 0; i < n / 2; ++i)
            heap.free(blocks[order[i]]);
        random_free += ns_per(t, n / 2);

        t = clock_type::now();
        for (std::size_t i = 0; i < n / 2; ++i)
            blocks[order[i]] = heap.alloc(plan[order[i]]);
        refill += ns_per(t, n / 2);

        t = clock_type::now();
        for (std::size_t i = 0; i < n; ++i)
            heap.free(blocks[i]);
        free_all += ns_per(t, n);
    }
    std::printf("%-7s alloc=%.1f  random_free=%.1f  refill_alloc=%.1f  free_all=%.1f  ns/op\n", name, fill / 5,
                random_free / 5, refill / 5, free_all / 5);
}
} // namespace

int main(int argc, char** argv)
{
    const double scale = argc > 1 ? std::atof(argv[1]) : 1.0;
    const auto n = static_cast<std::size_t>(1000000 * scale);
    std::printf("== allocator only, %zu blocks, 5 rounds\n", n);
    for (int round = 0; round < 2; ++round)
    {
        run<pool_heap<gc::detail::pool_mode::size_classes>>("class", n, 7);
        run<pool_heap<gc::detail::pool_mode::per_type>>("type", n, 7);
        run<system_heap>("system", n, 7);
    }
    return 0;
}
