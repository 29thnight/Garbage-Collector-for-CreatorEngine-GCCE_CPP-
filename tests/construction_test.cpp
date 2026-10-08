#include "support.hpp"

#include <stdexcept>
#include <string>

using gctest::live;
using gctest::node;

namespace
{
struct may_throw
{
    static inline int constructed = 0;
    static inline int destroyed = 0;
    gc::trace_ref<node> child;

    may_throw(gc::domain& d, bool fail)
    {
        child = gc::make<node>(d, 42); // completes before the failure
        if (fail)
            throw std::runtime_error("constructor failure");
        ++constructed;
    }
    ~may_throw() { ++destroyed; }
    void gc_trace(gc::tracer& t) const { t.visit(child); }
};

struct member_throws
{
    std::string text = "ok";
    struct thrower
    {
        thrower() { throw std::logic_error("member"); }
    } m;
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
    gc::collect_result result;
    gc::step_result step;
    explicit collects_in_constructor(gc::domain& d) : result(d.collect_full()), step(d.collect_step()) {}
};
} // namespace

// C07: a failing constructor releases its storage without running the
// destructor; objects it allocated become ordinary garbage.
TEST(Construction, ConstructorFailureReleasesStorage)
{
    gc::domain d;
    may_throw::constructed = 0;
    may_throw::destroyed = 0;

    EXPECT_THROW((void)gc::make<may_throw>(d, d, true), std::runtime_error);
    EXPECT_EQ(may_throw::destroyed, 0);
    EXPECT_EQ(live(d), 1u); // the nested node
    EXPECT_EQ(d.stats().roots, 0u);
    EXPECT_EQ(d.collect_full().reclaimed, 1u);

    auto ok = gc::make<may_throw>(d, d, false);
    EXPECT_EQ(may_throw::constructed, 1);
    d.collect_full();
    EXPECT_EQ(ok->child->id, 42);
}

TEST(Construction, MemberInitializerFailure)
{
    gc::domain d;
    EXPECT_THROW((void)gc::make<member_throws>(d), std::logic_error);
    EXPECT_EQ(live(d), 0u);
    // The reserved slot is reused cleanly.
    auto n = gc::make<node>(d, 1);
    gc::weak_ref<node> w = n;
    EXPECT_FALSE(w.expired());
}

TEST(Construction, NestedConstructionPublishesCompleteObjects)
{
    gc::domain d;
    auto p = gc::make<nested_parent>(d, d);
    EXPECT_EQ(d.stats().roots, 1u);
    d.collect_full();
    EXPECT_EQ(p->a->id, 1);
    EXPECT_EQ(p->b->id, 2);
    EXPECT_EQ(live(d), 3u);
}

TEST(Construction, CollectionFromConstructorIsRefusedAndRequested)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto c = gc::make<collects_in_constructor>(d, d);
    EXPECT_TRUE(c->result.refused);
    EXPECT_TRUE(c->step.refused);
    EXPECT_EQ(rec.count(gc::violation_kind::nested_collection), 2u);
    EXPECT_TRUE(d.collection_requested());
    EXPECT_TRUE(d.collect_full().completed);
}

namespace
{
gc::domain* g_domain = nullptr;

struct allocates_in_trace
{
    void gc_trace(gc::tracer&) const { (void)gc::make<node>(*g_domain, 0); }
};

struct collects_in_destructor
{
    gc::domain* d;
    ~collects_in_destructor() { (void)d->collect_full(); }
};

struct republishes_itself
{
    gc::trace_ref<republishes_itself> self;
    ~republishes_itself() { gc::root_ref<republishes_itself> resurrect = self; }
    void gc_trace(gc::tracer& t) const { t.visit(self); }
};
} // namespace

TEST(Construction, AllocationInsideTraceIsReported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    g_domain = &d;
    auto a = gc::make<allocates_in_trace>(d);
    d.collect_full();
    EXPECT_EQ(rec.count(gc::violation_kind::allocation_during_trace), 1u);
    a = nullptr;
    g_domain = nullptr;
}

TEST(Construction, CollectionInsideDestructorIsReported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    (void)gc::make<collects_in_destructor>(d, &d);
    EXPECT_TRUE(d.collect_full().completed);
    EXPECT_EQ(rec.count(gc::violation_kind::nested_collection), 1u);
}

TEST(Construction, RepublishingAReclaimedObjectIsReported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    {
        auto a = gc::make<republishes_itself>(d);
        a->self = a;
    }
    d.collect_full();
    EXPECT_EQ(rec.count(gc::violation_kind::store_of_condemned_object), 1u);
}

TEST(ConstructionDeathTest, DefaultHandlerAborts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            gc::domain d;
            (void)gc::make<collects_in_destructor>(d, &d);
            d.collect_full();
        },
        "nested_collection");
}
