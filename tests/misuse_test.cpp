// Debug checks (GC_DEBUG_CHECKS) for misuse that the type system cannot
// reject. In builds without debug checks only the behavior is checked.

#include "support.hpp"

#include <mutex>
#include <vector>

using gctest::node;

namespace
{
// Records process-wide violations for the duration of a test.
class global_recorder
{
public:
    global_recorder()
    {
        gc::set_global_violation_handler([this](const gc::violation& v) {
            std::lock_guard lock(mutex_);
            kinds_.push_back(v.kind);
        });
    }
    ~global_recorder() { gc::set_global_violation_handler({}); }
    global_recorder(const global_recorder&) = delete;
    global_recorder& operator=(const global_recorder&) = delete;

    std::size_t count(gc::violation_kind kind)
    {
        std::lock_guard lock(mutex_);
        std::size_t n = 0;
        for (auto k : kinds_)
            n += k == kind;
        return n;
    }

private:
    std::mutex mutex_;
    std::vector<gc::violation_kind> kinds_;
};

struct widget : gc::managed
{
    int value = 1;
};

// A GC type held by value inside another GC type.
struct embeds_widget : gc::managed
{
    widget inner;
};

struct holds_root : gc::managed
{
    gc::root_ref<node> pinned; // a root inside a GC object
};

struct big_holds_root : gc::managed
{
    unsigned char payload[4096]; // above the pooled sizes
    gc::root_ref<node> pinned;
};

struct plain_holder
{
    gc::root_ref<node> fine; // a root in a non-GC object is allowed
};
} // namespace

#if GC_DEBUG_CHECKS
constexpr bool checks = true;
#else
constexpr bool checks = false;
#endif

TEST(Misuse, InstancesOutsideMakeHaveNoIdentity)
{
    global_recorder rec;
    gc::domain d;
    auto w = gc::make<widget>(d);
    EXPECT_EQ(rec.count(gc::violation_kind::created_outside_make), 0u);

    widget copy = *w; // copy onto the stack
    EXPECT_FALSE(copy.root_from_this());
    EXPECT_TRUE(copy.weak_from_this().expired());

    widget local;
    EXPECT_FALSE(local.root_from_this());

    EXPECT_EQ(rec.count(gc::violation_kind::created_outside_make), checks ? 2u : 0u);

    *w = local; // assignment never transfers identity, and is not a construction
    EXPECT_TRUE(w->root_from_this() == w);
    EXPECT_EQ(rec.count(gc::violation_kind::created_outside_make), checks ? 2u : 0u);
}

TEST(Misuse, GcTypeHeldByValueInsideAGcObject)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto e = gc::make<embeds_widget>(d);
    EXPECT_EQ(rec.count(gc::violation_kind::created_outside_make), checks ? 1u : 0u);
    EXPECT_TRUE(e->root_from_this() == e);
}

TEST(Misuse, NestedMakeIsNotAViolation)
{
    struct outer : gc::managed
    {
        gc::trace_ref<widget> a;
        gc::trace_ref<widget> b;
        explicit outer(gc::domain& d) : a(gc::make<widget>(d)), b(gc::make<widget>(d)) {}
    };
    global_recorder global;
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto o = gc::make<outer>(d, d);
    EXPECT_EQ(rec.total(), 0u);
    EXPECT_EQ(global.count(gc::violation_kind::created_outside_make), 0u);
}

TEST(Misuse, RootInsideAGcObject)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto h = gc::make<holds_root>(d);
    h->pinned = gc::make<node>(d, 1);
    EXPECT_EQ(rec.count(gc::violation_kind::root_inside_gc_object), checks ? 1u : 0u);

    auto big = gc::make<big_holds_root>(d);
    big->pinned = gc::make<node>(d, 2);
    EXPECT_EQ(rec.count(gc::violation_kind::root_inside_gc_object), checks ? 2u : 0u);

    // Roots elsewhere are fine.
    plain_holder on_stack;
    on_stack.fine = gc::make<node>(d, 3);
    std::vector<gc::root_ref<node>> in_vector{gc::make<node>(d, 4)};
    EXPECT_EQ(rec.count(gc::violation_kind::root_inside_gc_object), checks ? 2u : 0u);

    h->pinned = nullptr;
    big->pinned = nullptr;
}

TEST(Misuse, WeakRefUsedAfterItsDomainIsGone)
{
    if (!checks)
        GTEST_SKIP() << "use after the domain is gone is undefined without debug checks";
    global_recorder rec;
    gc::weak_ref<node> stale;
    {
        gc::domain d;
        auto n = gc::make<node>(d, 1);
        stale = n;
        EXPECT_FALSE(stale.expired());
    }
    EXPECT_FALSE(stale.lock());
    EXPECT_TRUE(stale.expired());
    EXPECT_EQ(rec.count(gc::violation_kind::weak_ref_outlived_domain), 2u);
}

TEST(MisuseDeathTest, DefaultGlobalHandlerAborts)
{
    if (!checks)
        GTEST_SKIP();
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ widget w; (void)w; }, "created_outside_make");
}
