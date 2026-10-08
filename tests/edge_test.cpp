// States that take billions of cycles or slot reuses to reach, forced with
// the testing hooks.

#include "support.hpp"

#include <gc/detail/testing.hpp>

#include <limits>
#include <vector>

using gctest::live;
using gctest::node;
using testing_hooks = gc::detail::domain_testing;

constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();

// The mark epoch wraps from its maximum back to 1; marks from before the
// wrap must not make garbage look marked afterwards.
TEST(Edge, MarkEpochWrap)
{
    gc::domain d;
    testing_hooks::set_mark_epoch(d, u32_max - 3);

    auto root = gc::make<node>(d, 0);
    for (int i = 0; i < 10; ++i)
        root->children.push_back(gc::make<node>(d, i));

    for (int cycle = 0; cycle < 8; ++cycle)
    {
        // Garbage made in several ways: unreachable now, dropped from the
        // graph, and allocated during the sweep of an incremental cycle.
        for (int i = 0; i < 20; ++i)
            (void)gc::make<node>(d, -1);
        root->children.pop_back();
        root->children.push_back(gc::make<node>(d, 100 + cycle));

        auto r = d.collect_full();
        ASSERT_TRUE(r.completed) << "cycle " << cycle;
        EXPECT_EQ(r.reclaimed, 21u) << "cycle " << cycle;
        EXPECT_EQ(live(d), 11u) << "cycle " << cycle;
        for (const auto& c : root->children)
            ASSERT_TRUE(c);
    }
    EXPECT_GE(testing_hooks::mark_epoch(d), 1u);
    EXPECT_LT(testing_hooks::mark_epoch(d), 10u); // it wrapped

    // Incremental cycles across the wrap, with objects created mid-sweep.
    testing_hooks::set_mark_epoch(d, u32_max - 1);
    for (int cycle = 0; cycle < 4; ++cycle)
    {
        for (int i = 0; i < 50; ++i)
            (void)gc::make<node>(d, -1);
        gctest::step_until_finalized(d);
        std::vector<gc::weak_ref<node>> made_in_sweep;
        while (d.current_phase() == gc::phase::sweeping)
        {
            made_in_sweep.push_back(gc::make<node>(d, -2));
            d.collect_step(gctest::one_unit);
        }
        for (const auto& w : made_in_sweep)
            EXPECT_FALSE(w.expired());
        d.collect_full();
        for (const auto& w : made_in_sweep)
            EXPECT_TRUE(w.expired()) << "cycle " << cycle;
        EXPECT_EQ(live(d), 11u);
    }
}

// A slot whose generation reaches the maximum is retired instead of being
// reused, so no weak_ref can ever match a later object in it.
TEST(Edge, SlotGenerationRetirement)
{
    gc::domain d;
    auto victim = gc::make<node>(d, 1);
    const std::uint32_t slot = testing_hooks::slot_of(victim);
    testing_hooks::set_slot_generation(d, slot, u32_max);
    gc::weak_ref<node> weak = victim; // captures generation u32_max
    EXPECT_FALSE(weak.expired());

    victim = nullptr;
    d.collect_full();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(testing_hooks::slot_generation(d, slot), u32_max); // not bumped: retired

    std::vector<gc::root_ref<node>> later;
    for (int i = 0; i < 1000; ++i)
    {
        later.push_back(gc::make<node>(d, i));
        ASSERT_NE(testing_hooks::slot_of(later.back()), slot);
    }
    EXPECT_TRUE(weak.expired());
}

// One step below the maximum: the slot is reused once more, then retired.
TEST(Edge, SlotGenerationReachesMaximumOnLastReuse)
{
    gc::domain d;
    auto first = gc::make<node>(d, 1);
    const std::uint32_t slot = testing_hooks::slot_of(first);
    testing_hooks::set_slot_generation(d, slot, u32_max - 1);
    gc::weak_ref<node> old = first;
    first = nullptr;
    d.collect_full();

    auto second = gc::make<node>(d, 2);
    ASSERT_EQ(testing_hooks::slot_of(second), slot);
    gc::weak_ref<node> current = second;
    EXPECT_TRUE(old.expired());
    EXPECT_EQ(current.lock()->id, 2);

    second = nullptr;
    d.collect_full();
    auto third = gc::make<node>(d, 3);
    EXPECT_NE(testing_hooks::slot_of(third), slot);
    EXPECT_TRUE(current.expired());
}
