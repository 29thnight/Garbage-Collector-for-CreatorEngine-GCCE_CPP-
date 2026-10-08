#include "test_framework.hpp"

#include <stdexcept>

using gctest::node;

namespace
{
struct throwing
{
    static inline int constructed = 0;
    static inline int destroyed = 0;
    gc::trace_ref<node> child;

    throwing(gc::domain& d, bool fail)
    {
        // A nested allocation completes before the outer constructor fails.
        child = gc::make<node>(d, 42);
        if (fail)
            throw std::runtime_error("constructor failure");
        ++constructed;
    }
    ~throwing() { ++destroyed; }
    void gc_trace(gc::tracer& t) const { t.visit(child); }
};

struct nested_parent
{
    gc::trace_ref<node> a;
    gc::trace_ref<node> b;
    explicit nested_parent(gc::domain& d) : a(gc::make<node>(d, 1)), b(gc::make<node>(d, 2)) {}
    void gc_trace(gc::tracer& t) const
    {
        t.visit(a);
        t.visit(b);
    }
};

struct collects_in_constructor
{
    explicit collects_in_constructor(gc::domain& d) { result = d.collect_full(); }
    gc::collect_result result;
};
} // namespace

// C07: a failing constructor releases its storage without running the
// destructor; objects it allocated become ordinary garbage.
GC_TEST(c07_constructor_failure)
{
    gc::domain d;
    throwing::constructed = 0;
    throwing::destroyed = 0;

    bool threw = false;
    try
    {
        (void)gc::make<throwing>(d, d, true);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(throwing::destroyed, 0);
    CHECK_EQ(d.stats().live_objects, std::size_t{1}); // the nested node

    auto r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{1});

    // The domain stays consistent after the failure.
    auto ok = gc::make<throwing>(d, d, false);
    CHECK_EQ(throwing::constructed, 1);
    d.collect_full();
    CHECK_EQ(ok->child->id, 42);
}

// C07: nested construction publishes each object only when complete.
GC_TEST(c07_nested_construction)
{
    gc::domain d;
    auto p = gc::make<nested_parent>(d, d);
    CHECK_EQ(d.stats().roots, std::size_t{1});
    d.collect_full();
    CHECK_EQ(p->a->id, 1);
    CHECK_EQ(p->b->id, 2);
    CHECK_EQ(d.stats().live_objects, std::size_t{3});
}

// A collection requested from a constructor is refused and turned into a
// request for the engine loop.
GC_TEST(collection_from_constructor_is_refused)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto c = gc::make<collects_in_constructor>(d, d);
    CHECK(c->result.refused);
    CHECK_EQ(rec.count(gc::violation_kind::nested_collection), std::size_t{1});
    CHECK(d.collection_requested());
    CHECK(d.collect_full().completed);
}

namespace
{
gc::domain* g_trace_domain = nullptr;

struct allocates_in_trace
{
    void gc_trace(gc::tracer&) const { (void)gc::make<node>(*g_trace_domain, 0); }
};

struct collects_in_destructor
{
    gc::domain* d;
    ~collects_in_destructor() { (void)d->collect_full(); }
};

struct stores_condemned
{
    gc::trace_ref<stores_condemned> peer;
    ~stores_condemned()
    {
        // Republishing a condemned object as a strong reference.
        gc::root_ref<stores_condemned> resurrect = peer;
    }
    void gc_trace(gc::tracer& t) const { t.visit(peer); }
};
} // namespace

GC_TEST(allocation_during_trace_is_reported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    g_trace_domain = &d;
    auto a = gc::make<allocates_in_trace>(d);
    d.collect_full();
    CHECK_EQ(rec.count(gc::violation_kind::allocation_during_trace), std::size_t{1});
    a = nullptr;
    g_trace_domain = nullptr;
}

GC_TEST(collection_from_destructor_is_reported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    (void)gc::make<collects_in_destructor>(d, &d);
    auto r = d.collect_full();
    CHECK(r.completed);
    CHECK_EQ(rec.count(gc::violation_kind::nested_collection), std::size_t{1});
}

GC_TEST(store_of_condemned_object_is_reported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    {
        // The destructor republishes its own (condemned) object as a root.
        auto a = gc::make<stores_condemned>(d);
        a->peer = a;
    }
    d.collect_full();
    CHECK_EQ(rec.count(gc::violation_kind::store_of_condemned_object), std::size_t{1});
}
