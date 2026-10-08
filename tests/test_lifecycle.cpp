#include "test_framework.hpp"

using gctest::node;

GC_TEST(lifecycle_transitions_are_forward_and_single_step)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    CHECK(gc::lifecycle_of(a) == gc::lifecycle_state::alive);

    CHECK(gc::advance_lifecycle(a, gc::lifecycle_state::destroy_requested));
    // A duplicate Destroy request is ignored.
    CHECK(!gc::advance_lifecycle(a, gc::lifecycle_state::destroy_requested));
    // Skipping a step or going back is refused.
    CHECK(!gc::advance_lifecycle(a, gc::lifecycle_state::destroyed));
    CHECK(!gc::advance_lifecycle(a, gc::lifecycle_state::alive));
    CHECK(gc::lifecycle_of(a) == gc::lifecycle_state::destroy_requested);

    CHECK(gc::advance_lifecycle(a, gc::lifecycle_state::destroying));
    CHECK(gc::advance_lifecycle(a, gc::lifecycle_state::destroyed));
    CHECK(!gc::begin_cleanup_obligation(a));
}

// C10: a destroyed object reachable from an external root is still traced,
// together with its strong subgraph, and reclaimed once the root is gone.
GC_TEST(c10_destroyed_object_keeps_strong_subgraph)
{
    gc::domain d;
    auto parent = gc::make<node>(d, 1);
    parent->children.push_back(gc::make<node>(d, 2));
    parent->children.front()->next = gc::make<node>(d, 3);

    CHECK(gc::begin_cleanup_obligation(parent));
    CHECK(gc::advance_lifecycle(parent, gc::lifecycle_state::destroy_requested));
    CHECK(gc::advance_lifecycle(parent, gc::lifecycle_state::destroying));
    CHECK(gc::advance_lifecycle(parent, gc::lifecycle_state::destroyed));

    auto r = d.collect_full();
    CHECK(r.completed);
    CHECK_EQ(r.reclaimed, std::size_t{0});
    CHECK_EQ(parent->children.front()->next->id, 3);

    parent = nullptr;
    r = d.collect_full();
    CHECK(r.completed);
    CHECK_EQ(r.reclaimed, std::size_t{3});
}

// An object whose engine cleanup has not finished must not become
// unreachable. The cycle is aborted before anything is reclaimed.
GC_TEST(unreachable_with_cleanup_obligation_aborts_cycle)
{
    gc::domain d;
    gctest::violation_recorder rec(d);

    auto registered = gc::make<node>(d, 1);
    CHECK(gc::begin_cleanup_obligation(registered));
    gc::weak_ref<node> handle = registered;
    (void)gc::make<node>(d, 2); // ordinary garbage

    for (auto state : {gc::lifecycle_state::alive, gc::lifecycle_state::destroy_requested,
                       gc::lifecycle_state::destroying})
    {
        if (state != gc::lifecycle_state::alive)
            CHECK(gc::advance_lifecycle(registered, state));
        gc::root_ref<node> keep = registered;
        registered = nullptr;

        auto before = rec.count(gc::violation_kind::unreachable_with_cleanup_obligation);
        keep = nullptr; // dropped before cleanup finished
        auto r = d.collect_full();
        CHECK(r.aborted);
        CHECK(!r.completed);
        CHECK_EQ(r.reclaimed, std::size_t{0});
        CHECK_EQ(rec.count(gc::violation_kind::unreachable_with_cleanup_obligation), before + 1);
        CHECK_EQ(d.stats().live_objects, std::size_t{2});

        registered = handle.lock(); // memory was kept, so promotion still works
        CHECK(registered);
    }

    // Finishing the cleanup discharges the obligation.
    CHECK(gc::advance_lifecycle(registered, gc::lifecycle_state::destroyed));
    registered = nullptr;
    auto r = d.collect_full();
    CHECK(r.completed);
    CHECK_EQ(r.reclaimed, std::size_t{2});
    CHECK_EQ(d.stats().cycles_aborted, std::uint64_t{3});
}

// The destroy-queue pattern: a root taken before the owning edge is cut keeps
// the object alive through cleanup, then releasing it allows reclamation.
GC_TEST(destroy_queue_root_protects_cleanup)
{
    gc::domain d;
    auto scene = gc::make<node>(d, 0);
    scene->children.push_back(gc::make<node>(d, 1));
    gc::root_ref<node> entity = scene->children.front();
    CHECK(gc::begin_cleanup_obligation(entity));

    // Destroy request: protect first, then detach from the scene.
    std::vector<gc::root_ref<node>> destroy_queue;
    destroy_queue.push_back(entity);
    entity = nullptr;
    CHECK(gc::advance_lifecycle(destroy_queue.front(), gc::lifecycle_state::destroy_requested));
    scene->children.clear();

    // A collection between request and cleanup keeps the object.
    CHECK(d.collect_full().completed);
    CHECK_EQ(d.stats().live_objects, std::size_t{2});

    CHECK(gc::advance_lifecycle(destroy_queue.front(), gc::lifecycle_state::destroying));
    CHECK(gc::advance_lifecycle(destroy_queue.front(), gc::lifecycle_state::destroyed));
    destroy_queue.clear();

    auto r = d.collect_full();
    CHECK(r.completed);
    CHECK_EQ(r.reclaimed, std::size_t{1});
}
