#include "support.hpp"

using gctest::node;

TEST(Shutdown, ReclaimsEverythingWithoutRoots)
{
    node::alive = 0;
    {
        gc::domain d;
        auto a = gc::make<node>(d, 1);
        a->next = gc::make<node>(d, 2);
        a->next->next = a;
        a = nullptr;
        EXPECT_EQ(node::alive, 2);
    }
    EXPECT_EQ(node::alive, 0);
}

TEST(Shutdown, FinishesACycleInProgress)
{
    node::alive = 0;
    {
        gc::domain d;
        {
            auto a = gc::make<node>(d, 1);
            for (int i = 0; i < 100; ++i)
                a->children.push_back(gc::make<node>(d, i));
            d.request_collection();
            d.collect_step(gctest::one_unit);
            ASSERT_EQ(d.current_phase(), gc::phase::marking);
        }
    }
    EXPECT_EQ(node::alive, 0);
}

// Roots that outlive the domain are unsupported: reported, detached, and
// the objects are leaked instead of being freed under live references.
TEST(Shutdown, RemainingRootsAreReportedAndDetached)
{
    gc::root_ref<node> survivor;
    std::size_t roots_reported = 0;
    std::size_t objects_reported = 0;
    {
        gc::domain d;
        d.set_violation_handler([&](const gc::violation& v) {
            roots_reported += v.kind == gc::violation_kind::roots_remaining_at_shutdown;
            objects_reported += v.kind == gc::violation_kind::objects_remaining_at_shutdown;
        });
        survivor = gc::make<node>(d, 1);
    }
    EXPECT_EQ(roots_reported, 1u);
    EXPECT_EQ(objects_reported, 1u);
    EXPECT_FALSE(survivor);
    survivor = nullptr; // detached: does not touch the destroyed domain
}

TEST(Shutdown, PendingCleanupIsReported)
{
    std::size_t obligations = 0;
    std::size_t remaining = 0;
    {
        gc::domain d;
        d.set_violation_handler([&](const gc::violation& v) {
            obligations += v.kind == gc::violation_kind::unreachable_with_cleanup_obligation;
            remaining += v.kind == gc::violation_kind::objects_remaining_at_shutdown;
        });
        auto a = gc::make<node>(d, 1);
        ASSERT_TRUE(gc::begin_cleanup_obligation(a));
    }
    EXPECT_EQ(obligations, 1u);
    EXPECT_EQ(remaining, 1u);
}
