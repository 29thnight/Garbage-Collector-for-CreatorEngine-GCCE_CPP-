#include "support.hpp"

#include <thread>
#include <type_traits>
#include <vector>

using gctest::live;
using gctest::node;

namespace
{
struct left_base
{
    int l = 1;
    virtual ~left_base() = default;
};
struct right_base
{
    int r = 2;
    virtual ~right_base() = default;
};
struct multiple_child : gc::managed, left_base, right_base
{
};

struct virtual_base : gc::managed
{
    int v = 3;
    virtual ~virtual_base() = default;
};
struct virtual_left : virtual virtual_base
{
};
struct virtual_right : virtual virtual_base
{
};
struct virtual_child : virtual_left, virtual_right
{
};

static_assert(std::is_trivially_copyable_v<gc::weak_ref<virtual_child>>);
static_assert(std::is_nothrow_copy_constructible_v<gc::weak_ref<virtual_child>>);
static_assert(std::is_nothrow_copy_assignable_v<gc::weak_ref<virtual_child>>);
static_assert(std::is_constructible_v<gc::weak_ref<const virtual_base>, const gc::weak_ref<virtual_child>&>);
static_assert(!std::is_constructible_v<gc::weak_ref<virtual_child>, const gc::weak_ref<const virtual_child>&>);
static_assert(!std::is_nothrow_constructible_v<gc::weak_ref<virtual_base>, const gc::weak_ref<virtual_child>&>);
} // namespace

TEST(WeakRef, DoesNotKeepAlive)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    EXPECT_FALSE(w.expired());
    EXPECT_EQ(w.lock()->id, 1);

    a = nullptr;
    d.collect_full();
    EXPECT_TRUE(w.expired());
    EXPECT_FALSE(w.lock());
}

TEST(WeakRef, EmptyAndReset)
{
    gc::domain d;
    gc::weak_ref<node> empty;
    EXPECT_TRUE(empty.expired());
    EXPECT_FALSE(empty.lock());

    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    w.reset();
    EXPECT_TRUE(w.expired());
}

TEST(WeakRef, LockReturnsARoot)
{
    gc::domain d;
    gc::weak_ref<node> w;
    {
        auto a = gc::make<node>(d, 1);
        w = a;
    }
    auto locked = w.lock(); // still alive: no collection ran
    ASSERT_TRUE(locked);
    d.collect_full();
    EXPECT_FALSE(w.expired());
    EXPECT_EQ(locked->id, 1);
}

// Memory protection and logical usability are separate: a destroyed object
// that is still reachable can be promoted.
TEST(WeakRef, PromotesLogicallyDestroyedObject)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    ASSERT_TRUE(gc::advance_lifecycle(a, gc::lifecycle_state::destroy_requested));
    ASSERT_TRUE(gc::advance_lifecycle(a, gc::lifecycle_state::destroying));
    ASSERT_TRUE(gc::advance_lifecycle(a, gc::lifecycle_state::destroyed));

    auto locked = w.lock();
    ASSERT_TRUE(locked);
    EXPECT_EQ(gc::lifecycle_of(locked), gc::lifecycle_state::destroyed);
}

// C09: a reused slot gets a new generation, so an old weak_ref never
// resolves to the object that now occupies the slot.
TEST(WeakRef, StaleWeakNeverResolvesToSlotReuse)
{
    gc::domain d;
    auto first = gc::make<node>(d, 1);
    gc::weak_ref<node> stale = first;
    first = nullptr;
    d.collect_full();

    auto second = gc::make<node>(d, 2); // takes the freed slot
    EXPECT_TRUE(stale.expired());
    EXPECT_FALSE(stale.lock());

    gc::weak_ref<node> fresh = second;
    EXPECT_EQ(fresh.lock()->id, 2);
}

TEST(WeakRef, ManyReusesOfOneSlot)
{
    gc::domain d;
    std::vector<gc::weak_ref<node>> history;
    for (int i = 0; i < 1000; ++i)
    {
        auto n = gc::make<node>(d, i);
        history.push_back(n);
        n = nullptr;
        d.collect_full();
    }
    for (const auto& w : history)
        EXPECT_TRUE(w.expired());
}

TEST(WeakRef, ConvertsToBase)
{
    struct base : gc::managed
    {
        int v = 5;
    };
    struct child : base
    {
    };

    gc::domain d;
    auto c = gc::make<child>(d);
    gc::weak_ref<child> wc = c;
    gc::weak_ref<base> wb = wc;
    EXPECT_EQ(wb.lock()->v, 5);
    c = nullptr;
    d.collect_full();
    EXPECT_TRUE(wb.expired());
}

TEST(WeakRef, ConvertsMultipleInheritanceWithAdjustedPointer)
{
    gc::domain d;
    auto c = gc::make<multiple_child>(d);
    gc::weak_ref<multiple_child> wc = c;
    gc::weak_ref<left_base> wl = wc;
    gc::weak_ref<right_base> wr = wc;
    gc::weak_ref<const right_base> assigned;
    assigned = wc;
    EXPECT_EQ(d.stats().roots, 1u); // conversion retains no root
    EXPECT_EQ(wl.lock().get(), static_cast<left_base*>(c.get()));
    EXPECT_EQ(wr.lock().get(), static_cast<right_base*>(c.get()));
    EXPECT_EQ(assigned.lock().get(), static_cast<const right_base*>(c.get()));
    EXPECT_EQ(wr.lock()->r, 2);

    c = nullptr;
    d.collect_full();
    EXPECT_TRUE(wl.expired());
    EXPECT_TRUE(wr.expired());
    EXPECT_TRUE(assigned.expired());
}

TEST(WeakRef, ConvertsVirtualBaseAndConst)
{
    gc::domain d;
    gc::weak_ref<virtual_child> empty;
    gc::weak_ref<virtual_base> empty_base = empty;
    EXPECT_TRUE(empty_base.expired());
    EXPECT_FALSE(empty_base.lock());

    auto c = gc::make<virtual_child>(d);
    gc::weak_ref<virtual_child> wc = c;
    gc::weak_ref<virtual_base> wb = wc;
    gc::weak_ref<const virtual_child> const_child = wc;
    gc::weak_ref<const virtual_base> const_base = const_child;
    gc::weak_ref<const virtual_base> assigned;
    assigned = wc;
    EXPECT_EQ(d.stats().roots, 1u);
    EXPECT_EQ(wb.lock().get(), static_cast<virtual_base*>(c.get()));
    EXPECT_EQ(const_child.lock().get(), c.get());
    EXPECT_EQ(const_base.lock().get(), static_cast<const virtual_base*>(c.get()));
    EXPECT_EQ(assigned.lock().get(), static_cast<const virtual_base*>(c.get()));
    EXPECT_EQ(wb.lock()->v, 3);

    c = nullptr;
    d.collect_full();
    EXPECT_TRUE(wb.expired());
    EXPECT_TRUE(const_child.expired());
    EXPECT_TRUE(const_base.expired());
    EXPECT_TRUE(assigned.expired());
}

TEST(WeakRef, ExpiredVirtualConversionIsEmpty)
{
    for (auto heap : {gc::heap_kind::size_class_pools, gc::heap_kind::system})
    {
        gc::weak_ref<virtual_base> converted;
        gc::weak_ref<virtual_base> assigned;
        gc::weak_ref<const virtual_child> const_child;
        gc::weak_ref<const virtual_base> const_base;
        {
            gc::domain d(gc::domain_config{heap});
            auto c = gc::make<virtual_child>(d);
            gc::weak_ref<virtual_child> stale = c;
            c = nullptr;
            d.collect_full();
            ASSERT_TRUE(stale.expired());

            // Reading the old vptr to adjust to virtual_base is a use-after-free.
            converted = gc::weak_ref<virtual_base>(stale);
            assigned = stale;
            const_child = stale;
            const_base = stale;
            EXPECT_FALSE(converted.lock());
            EXPECT_FALSE(assigned.lock());
            EXPECT_FALSE(const_child.lock());
            EXPECT_FALSE(const_base.lock());
            EXPECT_EQ(d.stats().roots, 0u);
        }
        // Failed conversions are empty, so no dead domain is consulted either.
        EXPECT_TRUE(converted.expired());
        EXPECT_TRUE(assigned.expired());
        EXPECT_TRUE(const_child.expired());
        EXPECT_TRUE(const_base.expired());
    }
}

namespace
{
struct prober : gc::managed
{
    static inline int promoted = 0;
    static inline int attempts = 0;
    gc::weak_ref<prober> other;
    ~prober()
    {
        ++attempts;
        if (other.lock())
            ++promoted;
    }
};
} // namespace

// C06: once objects are chosen for reclamation, no destructor running in the
// same sweep can promote any of them.
TEST(WeakRef, NoPromotionOfObjectsBeingReclaimed)
{
    gc::domain d;
    prober::promoted = 0;
    prober::attempts = 0;
    {
        auto a = gc::make<prober>(d);
        auto b = gc::make<prober>(d);
        a->other = b;
        b->other = a;
    }
    auto r = d.collect_full();
    EXPECT_EQ(r.reclaimed, 2u);
    EXPECT_EQ(prober::attempts, 2);
    EXPECT_EQ(prober::promoted, 0);
}

TEST(WeakRef, CopyingOnAnotherThreadNeedsNoRegistry)
{
    gc::domain d; // default handler: a registry access would abort
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    std::vector<gc::weak_ref<node>> copies;
    std::thread worker([&] {
        for (int i = 0; i < 100; ++i)
            copies.push_back(w);
    });
    worker.join();
    EXPECT_EQ(copies.size(), 100u);
    EXPECT_EQ(copies.back().lock()->id, 1);
}

TEST(WeakRef, CopyingExpiredVirtualReferenceOnAnotherThreadNeedsNoRegistry)
{
    gc::domain d;
    auto c = gc::make<virtual_child>(d);
    gc::weak_ref<virtual_child> stale = c;
    c = nullptr;
    d.collect_full();

    gc::weak_ref<virtual_child> assigned;
    std::thread worker([&] {
        gc::weak_ref<virtual_child> copy = stale;
        assigned = copy;
    });
    worker.join();
    EXPECT_TRUE(assigned.expired());
    EXPECT_FALSE(assigned.lock());
}

#if GC_DEBUG_CHECKS
TEST(WeakRef, CrossTypeConversionOffOwnerThreadIsReported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto c = gc::make<virtual_child>(d);
    gc::weak_ref<virtual_child> wc = c;
    gc::weak_ref<const virtual_child> converted;

    // Adding const is cross-type too. No collection overlaps this misuse;
    // the returning recorder reports it but deliberately permits continuation.
    std::thread worker([&] { converted = wc; });
    worker.join();
    EXPECT_GT(rec.count(gc::violation_kind::wrong_thread), 0u);
    EXPECT_EQ(d.stats().roots, 1u);
    EXPECT_EQ(converted.lock().get(), c.get());
}
#endif
