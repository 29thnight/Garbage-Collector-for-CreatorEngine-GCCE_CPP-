#include "support.hpp"

#include <vector>

using gctest::live;
using gctest::node;

namespace
{
void destroy(const gc::root_ref<node>& r)
{
    ASSERT_TRUE(gc::advance_lifecycle(r, gc::lifecycle_state::destroy_requested));
    ASSERT_TRUE(gc::advance_lifecycle(r, gc::lifecycle_state::destroying));
    ASSERT_TRUE(gc::advance_lifecycle(r, gc::lifecycle_state::destroyed));
}
} // namespace

TEST(Lifecycle, TransitionsMoveForwardOneStepAtATime)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    EXPECT_EQ(gc::lifecycle_of(a), gc::lifecycle_state::alive);

    EXPECT_TRUE(gc::advance_lifecycle(a, gc::lifecycle_state::destroy_requested));
    EXPECT_FALSE(gc::advance_lifecycle(a, gc::lifecycle_state::destroy_requested)); // duplicate
    EXPECT_FALSE(gc::advance_lifecycle(a, gc::lifecycle_state::destroyed));         // skip
    EXPECT_FALSE(gc::advance_lifecycle(a, gc::lifecycle_state::alive));             // back
    EXPECT_EQ(gc::lifecycle_of(a), gc::lifecycle_state::destroy_requested);

    EXPECT_TRUE(gc::advance_lifecycle(a, gc::lifecycle_state::destroying));
    EXPECT_TRUE(gc::advance_lifecycle(a, gc::lifecycle_state::destroyed));
    EXPECT_FALSE(gc::begin_cleanup_obligation(a));
}

// C10: a destroyed object reachable from a root is still traced together
// with its strong subgraph, and reclaimed after the root is gone.
TEST(Lifecycle, DestroyedObjectKeepsItsStrongSubgraph)
{
    gc::domain d;
    auto parent = gc::make<node>(d, 1);
    parent->children.push_back(gc::make<node>(d, 2));
    parent->children.front()->next = gc::make<node>(d, 3);
    ASSERT_TRUE(gc::begin_cleanup_obligation(parent));
    destroy(parent);

    EXPECT_EQ(d.collect_full().reclaimed, 0u);
    EXPECT_EQ(parent->children.front()->next->id, 3);

    parent = nullptr;
    EXPECT_EQ(d.collect_full().reclaimed, 3u);
}

// Protect with a root first, then cut the owning edge: the object survives
// collections between request and cleanup.
TEST(Lifecycle, ProtectThenDetachPattern)
{
    gc::domain d;
    auto owner = gc::make<node>(d, 0);
    owner->children.push_back(gc::make<node>(d, 1));
    std::vector<gc::root_ref<node>> pending;
    pending.push_back(owner->children.front());
    ASSERT_TRUE(gc::begin_cleanup_obligation(pending.front()));
    ASSERT_TRUE(gc::advance_lifecycle(pending.front(), gc::lifecycle_state::destroy_requested));
    owner->children.clear();

    EXPECT_TRUE(d.collect_full().completed);
    EXPECT_EQ(live(d), 2u);

    ASSERT_TRUE(gc::advance_lifecycle(pending.front(), gc::lifecycle_state::destroying));
    ASSERT_TRUE(gc::advance_lifecycle(pending.front(), gc::lifecycle_state::destroyed));
    pending.clear();
    EXPECT_EQ(d.collect_full().reclaimed, 1u);
}

// Default policy: the cycle that detects an unreachable object with an
// unfinished obligation is aborted; the object and its subgraph move to the
// quarantine; later cycles reclaim the other garbage again.
TEST(Lifecycle, QuarantineKeepsViolatorsAndLetsOtherGarbageGo)
{
    gc::domain d;
    gctest::violation_recorder rec(d);

    gc::weak_ref<node> violator;
    gc::weak_ref<node> violator_child;
    {
        auto v = gc::make<node>(d, 1);
        v->next = gc::make<node>(d, 2);
        ASSERT_TRUE(gc::begin_cleanup_obligation(v));
        violator = v;
        violator_child = v->next;
    }
    (void)gc::make<node>(d, 3); // ordinary garbage

    auto first = d.collect_full();
    EXPECT_TRUE(first.aborted);
    EXPECT_EQ(first.reclaimed, 0u);
    EXPECT_EQ(first.quarantined, 1u);
    EXPECT_EQ(rec.count(gc::violation_kind::unreachable_with_cleanup_obligation), 1u);
    EXPECT_EQ(d.stats().quarantined, 1u);

    auto second = d.collect_full();
    EXPECT_TRUE(second.completed);
    EXPECT_EQ(second.reclaimed, 1u); // node 3
    EXPECT_FALSE(violator.expired());
    EXPECT_FALSE(violator_child.expired());
    EXPECT_EQ(rec.total(), 1u); // reported once

    // Finishing the cleanup releases the quarantine.
    {
        auto v = violator.lock();
        ASSERT_TRUE(gc::advance_lifecycle(v, gc::lifecycle_state::destroy_requested));
        ASSERT_TRUE(gc::advance_lifecycle(v, gc::lifecycle_state::destroying));
        ASSERT_TRUE(gc::advance_lifecycle(v, gc::lifecycle_state::destroyed));
    }
    EXPECT_EQ(d.stats().quarantined, 0u);
    EXPECT_EQ(d.collect_full().reclaimed, 2u);
    EXPECT_EQ(live(d), 0u);
}

// Strict policy: every cycle aborts while the violation stands.
TEST(Lifecycle, StrictPolicyAbortsUntilResolved)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    d.set_obligation_policy(gc::obligation_policy::strict);

    gc::weak_ref<node> handle;
    {
        auto v = gc::make<node>(d, 1);
        ASSERT_TRUE(gc::begin_cleanup_obligation(v));
        handle = v;
    }
    (void)gc::make<node>(d, 2);

    for (int i = 0; i < 3; ++i)
    {
        auto r = d.collect_full();
        EXPECT_TRUE(r.aborted);
        EXPECT_EQ(r.reclaimed, 0u);
    }
    EXPECT_EQ(rec.count(gc::violation_kind::unreachable_with_cleanup_obligation), 3u);
    EXPECT_EQ(d.stats().cycles_aborted, 3u);
    EXPECT_EQ(d.stats().quarantined, 0u);

    destroy(handle.lock());
    EXPECT_EQ(d.collect_full().reclaimed, 2u);
}

TEST(Lifecycle, ObligationCheckedInEveryLogicalState)
{
    for (auto state : {gc::lifecycle_state::alive, gc::lifecycle_state::destroy_requested,
                       gc::lifecycle_state::destroying})
    {
        gc::domain d;
        gctest::violation_recorder rec(d);
        {
            auto v = gc::make<node>(d, 1);
            ASSERT_TRUE(gc::begin_cleanup_obligation(v));
            for (int s = 1; s <= static_cast<int>(state); ++s)
                ASSERT_TRUE(gc::advance_lifecycle(v, static_cast<gc::lifecycle_state>(s)));
        }
        EXPECT_TRUE(d.collect_full().aborted);
        EXPECT_EQ(rec.count(gc::violation_kind::unreachable_with_cleanup_obligation), 1u);
        d.set_violation_handler([](const gc::violation&) {}); // shutdown leaves it
    }
}
