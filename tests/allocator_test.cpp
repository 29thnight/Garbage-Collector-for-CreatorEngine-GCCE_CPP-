#include "support.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

using gctest::live;
using gctest::node;

namespace
{
template <std::size_t N>
struct sized : gc::managed
{
    unsigned char bytes[N];
};

struct alignas(64) aligned64 : gc::managed
{
    int v = 64;
};

constexpr std::size_t page = 64 * 1024;
} // namespace

TEST(Allocator, SmallObjectsComeFromPages)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    for (int i = 0; i < 10000; ++i)
        keep.push_back(gc::make<node>(d, i));
    auto st = d.stats();
    EXPECT_GE(st.heap_pages, 1u);
    EXPECT_EQ(st.heap_committed_bytes, st.heap_pages * page);
    EXPECT_GE(st.heap_committed_bytes, st.live_bytes);
    // Block rounding plus page headers stay within a modest overhead.
    EXPECT_LT(st.heap_committed_bytes, st.live_bytes * 2);
}

TEST(Allocator, FreedBlocksAreReusedWithoutGrowingTheHeap)
{
    gc::domain d;
    for (int i = 0; i < 5000; ++i)
        (void)gc::make<node>(d, i);
    d.collect_full();
    const auto pages_after_first = d.stats().heap_pages;

    for (int round = 0; round < 20; ++round)
    {
        for (int i = 0; i < 5000; ++i)
            (void)gc::make<node>(d, i);
        d.collect_full();
    }
    EXPECT_LE(d.stats().heap_pages, pages_after_first + 1);
}

TEST(Allocator, EmptyPagesAreReturned)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    for (int i = 0; i < 20000; ++i)
        keep.push_back(gc::make<node>(d, i));
    const auto peak_pages = d.stats().heap_pages;
    ASSERT_GT(peak_pages, 4u);

    keep.clear();
    d.collect_full();
    // At most one page with free space is kept per size class.
    EXPECT_LE(d.stats().heap_pages, 1u);
    EXPECT_EQ(live(d), 0u);
}

TEST(Allocator, MixedSizesAndLargeObjects)
{
    gc::domain d;
    std::vector<gc::root_ref<gc::managed>> keep;
    keep.push_back(gc::make<sized<1>>(d));
    keep.push_back(gc::make<sized<100>>(d));
    keep.push_back(gc::make<sized<1000>>(d));
    keep.push_back(gc::make<sized<1900>>(d));
    const auto small_committed = d.stats().heap_committed_bytes;

    auto large = gc::make<sized<100000>>(d); // above the largest class
    EXPECT_GE(d.stats().heap_committed_bytes, small_committed + 100000);
    large = nullptr;
    d.collect_full();
    EXPECT_EQ(d.stats().heap_committed_bytes, small_committed);
    EXPECT_EQ(live(d), 4u);
}

TEST(Allocator, OverAlignedObjectsKeepTheirAlignment)
{
    gc::domain d;
    std::vector<gc::root_ref<aligned64>> keep;
    for (int i = 0; i < 100; ++i)
    {
        keep.push_back(gc::make<aligned64>(d));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(keep.back().get()) % 64, 0u);
        EXPECT_EQ(keep.back()->v, 64);
    }
}

TEST(Allocator, DistinctLiveObjectsNeverShareStorage)
{
    gc::domain d;
    std::mt19937 rng(3);
    std::vector<gc::root_ref<node>> keep;
    for (int round = 0; round < 50; ++round)
    {
        for (int i = 0; i < 200; ++i)
            keep.push_back(gc::make<node>(d, i));
        std::shuffle(keep.begin(), keep.end(), rng);
        keep.resize(keep.size() / 2);
        d.collect_full();

        std::unordered_set<const node*> addresses;
        for (const auto& k : keep)
            EXPECT_TRUE(addresses.insert(k.get()).second);
    }
    EXPECT_EQ(live(d), keep.size());
}

TEST(Allocator, LifecycleOfManyObjectsWithIncrementalSteps)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    for (int frame = 0; frame < 300; ++frame)
    {
        for (int i = 0; i < 50; ++i)
        {
            auto n = gc::make<node>(d, i);
            if (i % 10 == 0)
                keep.push_back(n);
        }
        if (keep.size() > 200)
            keep.erase(keep.begin(), keep.begin() + 100);
        d.request_collection();
        d.collect_step({gc::duration::zero(), 64});
    }
    gctest::finish_cycle(d);
    d.collect_full();
    EXPECT_EQ(live(d), keep.size());
}
