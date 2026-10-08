#include "support.hpp"

#include <thread>
#include <vector>

using gctest::live;
using gctest::node;

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
    struct base
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

namespace
{
struct prober
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
