#include "support.hpp"
#include "../src/block_allocator_testing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
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

using allocator_failure = gc::detail::allocator_failure;

class allocator_fault_scope
{
public:
    gc::detail::allocator_test_state state;

    allocator_fault_scope() : previous_(gc::detail::set_allocator_test_state(&state)) {}
    ~allocator_fault_scope() { gc::detail::set_allocator_test_state(previous_); }

    allocator_fault_scope(const allocator_fault_scope&) = delete;
    allocator_fault_scope& operator=(const allocator_fault_scope&) = delete;

private:
    gc::detail::allocator_test_state* previous_;
};

class AllocatorFailures : public ::testing::TestWithParam<allocator_failure>
{
};
} // namespace

// Each injection precedes a genuinely throwing transaction step. Mapping
// counters distinguish a full rollback from merely hiding leaked storage.
TEST_P(AllocatorFailures, FirstAllocationRollsBackAndCanBeRetried)
{
    allocator_fault_scope fault;
    {
        gc::domain d;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            fault.state.fail_at = GetParam();
            EXPECT_THROW((void)gc::make<node>(d, 7), std::bad_alloc);
            EXPECT_EQ(fault.state.fail_at, allocator_failure::none);
            const auto after = d.stats();
            EXPECT_EQ(after.heap_chunks, 0u);
            EXPECT_EQ(after.heap_pages, 0u);
            EXPECT_EQ(after.heap_committed_bytes, 0u);
            EXPECT_EQ(after.live_objects, 0u);
            EXPECT_EQ(after.live_bytes, 0u);
            EXPECT_EQ(after.roots, 0u);
            EXPECT_EQ(fault.state.chunk_maps, fault.state.chunk_unmaps);
        }

        auto root = gc::make<node>(d, 42);
        EXPECT_EQ(root->id, 42);
        EXPECT_EQ(d.stats().heap_chunks, 1u);
        EXPECT_EQ(d.stats().heap_pages, 1u);
        EXPECT_EQ(fault.state.chunk_maps, fault.state.chunk_unmaps + 1);
        d.collect_full();
        EXPECT_EQ(live(d), 1u);
        root = nullptr;
        d.collect_full();
        EXPECT_EQ(live(d), 0u);
        EXPECT_EQ(d.stats().heap_pages, 0u);
    }
    EXPECT_EQ(fault.state.chunk_maps, fault.state.chunk_unmaps);
}

TEST_P(AllocatorFailures, GrowthFailurePreservesExistingStorage)
{
    allocator_fault_scope fault;
    {
        gc::domain d;
        std::vector<gc::root_ref<sized<1900>>> keep;
        keep.reserve(4096);
        keep.push_back(gc::make<sized<1900>>(d));
        keep.front()->bytes[0] = 73;
        fault.state.fail_at = GetParam();

        bool failed = false;
        // A page holds at most 34 objects of this size; 4096 is well past
        // one 32-page chunk, without depending on the private page header.
        for (int i = 0; i < 4095 && !failed; ++i)
        {
            const auto before = d.stats();
            try
            {
                keep.push_back(gc::make<sized<1900>>(d));
            }
            catch (const std::bad_alloc&)
            {
                failed = true;
                EXPECT_EQ(fault.state.fail_at, allocator_failure::none);
                const auto after = d.stats();
                EXPECT_EQ(after.heap_chunks, before.heap_chunks);
                EXPECT_EQ(after.heap_pages, before.heap_pages);
                EXPECT_EQ(after.heap_committed_bytes, before.heap_committed_bytes);
                EXPECT_EQ(after.live_objects, before.live_objects);
                EXPECT_EQ(after.live_bytes, before.live_bytes);
                EXPECT_EQ(after.roots, before.roots);
            }
        }
        ASSERT_TRUE(failed);
        EXPECT_EQ(fault.state.chunk_maps, fault.state.chunk_unmaps + 1);
        EXPECT_EQ(keep.front()->bytes[0], 73);
        keep.push_back(gc::make<sized<1900>>(d));
        d.collect_full();
        EXPECT_EQ(live(d), keep.size());
        EXPECT_EQ(keep.front()->bytes[0], 73);
        keep.clear();
        d.collect_full();
        EXPECT_EQ(live(d), 0u);
        EXPECT_EQ(d.stats().heap_pages, 0u);
    }
    EXPECT_EQ(fault.state.chunk_maps, fault.state.chunk_unmaps);
}

INSTANTIATE_TEST_SUITE_P(TransactionSteps, AllocatorFailures,
                        ::testing::Values(allocator_failure::chunk_metadata,
                                          allocator_failure::chunk_mapping,
                                          allocator_failure::chunk_vector,
                                          allocator_failure::chunk_index,
                                          allocator_failure::page_commit));

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
    EXPECT_EQ(d.stats().heap_pages, 0u);
    EXPECT_EQ(live(d), 0u);
}

// Freed pages stay committed for reuse, then give their physical memory back
// after staying idle for two cycles; idle chunks are unmapped.
TEST(Allocator, MemoryGoesBackToTheOs)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    for (int i = 0; i < 50000; ++i)
        keep.push_back(gc::make<node>(d, i));
    const auto full = d.stats();
    ASSERT_GT(full.heap_chunks, 1u);

    keep.clear();
    d.collect_full();
    EXPECT_EQ(d.stats().heap_pages, 0u);
    EXPECT_GE(d.stats().heap_committed_bytes, full.heap_committed_bytes / 2); // kept warm for reuse

    d.collect_full();
    d.collect_full();
    const auto after = d.stats();
    EXPECT_LE(after.heap_committed_bytes, 8u * page);
    EXPECT_LE(after.heap_chunks, 1u);
    EXPECT_LT(after.heap_committed_bytes, full.heap_committed_bytes / 10);
}

// An emptied page is not tied to its size class.
TEST(Allocator, EmptyPagesServeOtherSizes)
{
    gc::domain d;
    std::vector<gc::root_ref<gc::managed>> keep;
    for (int i = 0; i < 20000; ++i)
        keep.push_back(gc::make<sized<40>>(d));
    const auto chunks_small = d.stats().heap_chunks;
    keep.clear();
    d.collect_full();

    for (int i = 0; i < 2000; ++i)
        keep.push_back(gc::make<sized<400>>(d));
    EXPECT_LE(d.stats().heap_chunks, chunks_small);
}

namespace
{
struct payload_node : gc::managed
{
    gc::trace_ref<payload_node> next;
    std::uint32_t pattern;
    unsigned char bytes[200];
    explicit payload_node(std::uint32_t p) : pattern(p) { std::memset(bytes, static_cast<int>(p & 0xFF), sizeof(bytes)); }
    bool intact() const
    {
        for (unsigned char b : bytes)
            if (b != static_cast<unsigned char>(pattern & 0xFF))
                return false;
        return true;
    }
    void gc_trace(gc::tracer& t) const { t.visit(next); }
};

class HeapKinds : public ::testing::TestWithParam<gc::heap_kind>
{
};
} // namespace

// The same workload on every heap: objects keep their contents across
// collections, and live counts match.
TEST_P(HeapKinds, WorkloadKeepsObjectsIntact)
{
    gc::domain d(gc::domain_config{GetParam()});
    std::mt19937 rng(11);
    std::vector<gc::root_ref<payload_node>> keep;
    std::uint32_t next_pattern = 1;
    for (int round = 0; round < 30; ++round)
    {
        for (int i = 0; i < 2000; ++i)
        {
            auto n = gc::make<payload_node>(d, next_pattern++);
            if (rng() % 4 == 0)
                keep.push_back(n);
            else if (!keep.empty())
                keep[rng() % keep.size()]->next = n;
        }
        std::shuffle(keep.begin(), keep.end(), rng);
        keep.resize(keep.size() * 2 / 3);
        d.request_collection();
        while (!d.collect_step({gc::duration::zero(), 128}).cycle_finished)
        {
        }
        for (const auto& k : keep)
        {
            ASSERT_TRUE(k->intact());
            if (k->next)
            {
                ASSERT_TRUE(k->next->intact());
            }
        }
    }
    keep.clear();
    d.collect_full();
    EXPECT_EQ(live(d), 0u);
}

INSTANTIATE_TEST_SUITE_P(All, HeapKinds,
                         ::testing::Values(gc::heap_kind::size_class_pools, gc::heap_kind::system));

#if GC_DEBUG_CHECKS
TEST_P(HeapKinds, IndividualTrackingFailureRollsBackAndCanBeRetried)
{
    allocator_fault_scope fault;
    gc::domain d(gc::domain_config{GetParam()});
    auto existing = gc::make<node>(d, 19);
    const auto before = d.stats();

    // Large allocations exercise plain new in system mode and aligned new
    // in pooled mode; the over-aligned case always uses aligned new.
    fault.state.fail_at = allocator_failure::individual_index;
    EXPECT_THROW((void)gc::make<sized<100000>>(d), std::bad_alloc);
    EXPECT_EQ(fault.state.fail_at, allocator_failure::none);
    EXPECT_EQ(d.stats().heap_committed_bytes, before.heap_committed_bytes);
    EXPECT_EQ(d.stats().live_objects, before.live_objects);
    EXPECT_EQ(d.stats().live_bytes, before.live_bytes);

    fault.state.fail_at = allocator_failure::individual_index;
    EXPECT_THROW((void)gc::make<aligned64>(d), std::bad_alloc);
    EXPECT_EQ(fault.state.fail_at, allocator_failure::none);
    EXPECT_EQ(d.stats().heap_committed_bytes, before.heap_committed_bytes);
    EXPECT_EQ(d.stats().live_objects, before.live_objects);
    EXPECT_EQ(d.stats().roots, before.roots);

    auto large = gc::make<sized<100000>>(d);
    auto aligned = gc::make<aligned64>(d);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned.get()) % 64, 0u);
    EXPECT_EQ(aligned->v, 64);
    EXPECT_EQ(existing->id, 19);
    d.collect_full();
    EXPECT_EQ(live(d), 3u);
    large = nullptr;
    aligned = nullptr;
    d.collect_full();
    EXPECT_EQ(d.stats().heap_committed_bytes, before.heap_committed_bytes);
    EXPECT_EQ(live(d), 1u);
}
#endif

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
