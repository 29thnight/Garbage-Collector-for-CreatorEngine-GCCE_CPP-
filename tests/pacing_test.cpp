#include "support.hpp"

#include <vector>

using gctest::finish_cycle;
using gctest::node;

namespace
{
struct blob : gc::managed
{
    unsigned char bytes[1024];
};

std::size_t block_size_of_blob()
{
    gc::domain probe;
    (void)gc::make<blob>(probe);
    return probe.stats().live_bytes;
}
} // namespace

TEST(Pacing, RequestsACycleWhenLiveBytesPassStartFraction)
{
    const std::size_t block = block_size_of_blob();
    gc::domain d;
    d.set_allocation_threshold(static_cast<std::size_t>(-1)); // isolate pacing
    d.set_pacing({block * 100, 0.5, 4.0});

    std::vector<gc::root_ref<blob>> keep;
    for (int i = 0; i < 49; ++i)
        keep.push_back(gc::make<blob>(d));
    EXPECT_FALSE(d.collection_requested());
    keep.push_back(gc::make<blob>(d));
    EXPECT_TRUE(d.collection_requested());
}

TEST(Pacing, BudgetGrowsWithMemoryPressure)
{
    const std::size_t block = block_size_of_blob();
    gc::domain d;
    d.set_allocation_threshold(static_cast<std::size_t>(-1));
    d.set_pacing({block * 100, 0.5, 5.0});

    std::vector<gc::root_ref<blob>> keep;
    for (int i = 0; i < 40; ++i)
        keep.push_back(gc::make<blob>(d));
    d.request_collection();
    auto low = d.collect_step({gc::duration::zero(), 1});
    EXPECT_DOUBLE_EQ(low.budget_scale, 1.0);
    finish_cycle(d);

    for (int i = 0; i < 35; ++i) // 75% of the limit: halfway into the ramp
        keep.push_back(gc::make<blob>(d));
    d.request_collection();
    auto mid = d.collect_step({gc::duration::zero(), 4});
    EXPECT_NEAR(mid.budget_scale, 3.0, 0.1);
    EXPECT_EQ(mid.units, 12u); // min_units scaled by 3
    finish_cycle(d);

    for (int i = 0; i < 30; ++i) // over the limit
        keep.push_back(gc::make<blob>(d));
    d.request_collection();
    auto high = d.collect_step({gc::duration::zero(), 1});
    EXPECT_DOUBLE_EQ(high.budget_scale, 5.0);
    finish_cycle(d);

    auto st = d.stats();
    EXPECT_EQ(st.memory_limit, block * 100);
    EXPECT_GE(st.allocations_over_limit, 5u);
    EXPECT_GE(st.peak_live_bytes, block * 105);
}

TEST(Pacing, DisabledByDefault)
{
    gc::domain d;
    std::vector<gc::root_ref<blob>> keep;
    for (int i = 0; i < 100; ++i)
        keep.push_back(gc::make<blob>(d));
    d.request_collection();
    EXPECT_DOUBLE_EQ(d.collect_step().budget_scale, 1.0);
    EXPECT_EQ(d.stats().memory_limit, 0u);
    EXPECT_EQ(d.stats().allocations_over_limit, 0u);
}
