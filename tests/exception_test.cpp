#include "support.hpp"

#include <gc/detail/testing.hpp>

#include <new>
#include <stdexcept>

namespace
{
using gc::detail::collection_failure;
using testing_hooks = gc::detail::domain_testing;

struct throwing_node : gc::managed
{
    gc::trace_ref<gctest::node> first;
    gc::trace_ref<gctest::node> second;
    bool fail = true;

    void gc_trace(gc::tracer& t) const
    {
        t.visit(first); // leave an edge buffered before throwing
        if (fail)
            throw std::runtime_error("trace failed");
        t.visit(second);
    }
};

void collect_until_idle(gc::domain& d, bool full)
{
    if (full)
        (void)d.collect_full();
    else
    {
        d.request_collection();
        do
        {
            (void)d.collect_step(gctest::one_unit);
        } while (d.current_phase() != gc::phase::idle);
    }
}
} // namespace

TEST(Exceptions, FailedTraceAbandonsPartialMarkAndRetriesFromRoots)
{
    for (const bool full : {false, true})
    {
        gc::domain d;
        gctest::violation_recorder rec(d);
        auto root = gc::make<throwing_node>(d);
        root->first = gc::make<gctest::node>(d, 1);
        root->second = gc::make<gctest::node>(d, 2);
        gc::weak_ref<gctest::node> first = root->first;
        gc::weak_ref<gctest::node> second = root->second;
        gc::weak_ref<gctest::node> garbage = gc::make<gctest::node>(d, 3);

        EXPECT_THROW(collect_until_idle(d, full), std::runtime_error);
        root->fail = false; // also keeps domain destruction nonthrowing
        EXPECT_EQ(d.current_phase(), gc::phase::idle);
        EXPECT_TRUE(d.collection_requested());
        EXPECT_TRUE(d.stats().last.aborted);
        EXPECT_EQ(d.stats().last.reclaimed, 0u);
        EXPECT_EQ(d.stats().cycles_aborted, 1u);
        EXPECT_EQ(gctest::live(d), 4u);
        EXPECT_FALSE(first.expired());
        EXPECT_FALSE(second.expired());
        EXPECT_FALSE(garbage.expired());

        // An allocation proves tracing was cleared. A new cycle must trace
        // the popped root again, including the edge after the failed visit.
        auto extra = gc::make<gctest::node>(d, 4);
        const auto retry = d.collect_step(gctest::one_unit);
        EXPECT_TRUE(retry.cycle_started); // no explicit request needed
        EXPECT_FALSE(retry.refused);
        gctest::finish_cycle(d);
        EXPECT_TRUE(d.stats().last.completed);
        EXPECT_EQ(d.stats().last.reclaimed, 1u);
        EXPECT_FALSE(first.expired());
        EXPECT_FALSE(second.expired());
        EXPECT_TRUE(garbage.expired());
        EXPECT_EQ(rec.total(), 0u);
    }
}

TEST(Exceptions, CollectionAllocationFailuresLeaveAUsableDomain)
{
    for (const auto point : {collection_failure::gray_reserve,
                             collection_failure::suspects_growth,
                             collection_failure::quarantine_growth})
    {
        for (const bool full : {false, true})
        {
            gc::domain d;
            gctest::violation_recorder rec(d);
            auto root = gc::make<gctest::node>(d, 0);
            gc::weak_ref<gctest::node> obligation;
            gc::weak_ref<gctest::node> child;
            {
                auto v = gc::make<gctest::node>(d, 1);
                v->next = gc::make<gctest::node>(d, 2);
                child = v->next;
                ASSERT_TRUE(gc::begin_cleanup_obligation(v));
                obligation = v;
            }
            gc::weak_ref<gctest::node> garbage = gc::make<gctest::node>(d, 3);
            testing_hooks::fail_next_collection_allocation(d, point);
            EXPECT_THROW(collect_until_idle(d, full), std::bad_alloc);
            testing_hooks::fail_next_collection_allocation(d, collection_failure::none);
            EXPECT_EQ(d.current_phase(), gc::phase::idle);
            EXPECT_TRUE(d.collection_requested());
            EXPECT_EQ(d.stats().cycles_aborted, 1u);
            EXPECT_TRUE(d.stats().last.aborted);
            EXPECT_EQ(d.stats().last.reclaimed, 0u);
            EXPECT_EQ(d.stats().quarantined, 0u);
            EXPECT_EQ(gctest::live(d), 4u);

            // Retry must insert the obligation into quarantine, including
            // after failure between deciding to quarantine and publication.
            EXPECT_TRUE(d.collect_full().aborted);
            EXPECT_EQ(d.stats().quarantined, 1u);
            EXPECT_TRUE(d.collect_full().completed);
            EXPECT_FALSE(obligation.expired());
            EXPECT_FALSE(child.expired());
            EXPECT_TRUE(garbage.expired());
            {
                auto v = obligation.lock();
                ASSERT_TRUE(v);
                ASSERT_TRUE(gc::advance_lifecycle(v, gc::lifecycle_state::destroy_requested));
                ASSERT_TRUE(gc::advance_lifecycle(v, gc::lifecycle_state::destroying));
                ASSERT_TRUE(gc::advance_lifecycle(v, gc::lifecycle_state::destroyed));
            }
            EXPECT_EQ(d.collect_full().reclaimed, 2u);
            EXPECT_EQ(d.stats().quarantined, 0u);
            EXPECT_EQ(rec.count(gc::violation_kind::nested_collection), 0u);
            EXPECT_EQ(rec.count(gc::violation_kind::allocation_during_trace), 0u);
        }
    }
}

TEST(Exceptions, FailedDiagnosticTraceRestoresTracingFlag)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto root = gc::make<throwing_node>(d);
    root->first = gc::make<gctest::node>(d, 1);
    root->second = gc::make<gctest::node>(d, 2);
    EXPECT_THROW(gc::retention_path(root->second), std::runtime_error);
    root->fail = false;
    auto extra = gc::make<gctest::node>(d, 3);
    EXPECT_TRUE(d.collect_full().completed);
    EXPECT_EQ(gc::retention_path(root->second).size(), 2u);
    EXPECT_EQ(rec.total(), 0u);
}
