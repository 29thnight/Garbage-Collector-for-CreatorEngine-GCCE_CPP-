#include "test_framework.hpp"

#include <thread>

using gctest::node;

#if GC_THREAD_CHECKS
// Root registration and reference stores are owner-thread operations.
GC_TEST(wrong_thread_root_change_is_reported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a; // copying weak refs needs no registry access

    std::thread worker([&] {
        gc::weak_ref<node> copy = w; // allowed
        gc::root_ref<node> r = a;    // link: violation
        (void)copy;
    }); // unlink: violation
    worker.join();
    CHECK_EQ(rec.count(gc::violation_kind::wrong_thread), std::size_t{3}); // store, link, unlink

    // After rebinding, the worker thread becomes the owner.
    std::thread rebind([&] {
        d.bind_to_current_thread();
        gc::root_ref<node> r = a;
        d.collect_full();
    });
    rebind.join();
    CHECK_EQ(rec.count(gc::violation_kind::wrong_thread), std::size_t{3});
    d.bind_to_current_thread();
}
#endif

GC_TEST(shutdown_reclaims_unrooted_objects)
{
    node::alive = 0;
    {
        gc::domain d;
        auto a = gc::make<node>(d, 1);
        a->next = gc::make<node>(d, 2);
        a->next->next = a;
        a = nullptr;
        CHECK_EQ(node::alive, 2);
    }
    CHECK_EQ(node::alive, 0);
}

// Roots that outlive the domain are unsupported: reported, detached and the
// objects leaked rather than freed under live references.
GC_TEST(shutdown_with_remaining_roots_is_reported)
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
    CHECK_EQ(roots_reported, std::size_t{1});
    CHECK_EQ(objects_reported, std::size_t{1});
    CHECK(!survivor);
    survivor = nullptr; // detached root: no access to the destroyed domain
}

// Objects whose cleanup never finished are not destroyed at shutdown.
GC_TEST(shutdown_with_pending_cleanup_is_reported)
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
        CHECK(gc::begin_cleanup_obligation(a));
    }
    CHECK_EQ(obligations, std::size_t{1});
    CHECK_EQ(remaining, std::size_t{1});
}
