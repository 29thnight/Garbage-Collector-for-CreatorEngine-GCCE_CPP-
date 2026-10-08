#include "test_framework.hpp"

#include <vector>

using gctest::node;

GC_TEST(weak_does_not_keep_alive)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    CHECK(!w.expired());
    CHECK_EQ(w.lock()->id, 1);

    a = nullptr;
    d.collect_full();
    CHECK(w.expired());
    CHECK(!w.lock());

    gc::weak_ref<node> empty;
    CHECK(empty.expired());
    CHECK(!empty.lock());
}

// Promotion yields a root even for a logically destroyed object: memory
// protection and engine usability are separate questions.
GC_TEST(weak_promotes_logically_destroyed_object)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    CHECK(gc::advance_lifecycle(a, gc::lifecycle_state::destroy_requested));
    CHECK(gc::advance_lifecycle(a, gc::lifecycle_state::destroying));
    CHECK(gc::advance_lifecycle(a, gc::lifecycle_state::destroyed));

    auto locked = w.lock();
    CHECK(locked);
    CHECK(gc::lifecycle_of(locked) == gc::lifecycle_state::destroyed);
}

// C08 / C09: a reused slot gets a new generation, so an old weak_ref never
// resolves to the new object even at the same slot or address.
GC_TEST(c09_stale_weak_after_slot_reuse)
{
    gc::domain d;
    auto first = gc::make<node>(d, 1);
    gc::weak_ref<node> stale = first;
    first = nullptr;
    d.collect_full();
    CHECK(stale.expired());

    // The freed slot is reused by the next allocation.
    auto second = gc::make<node>(d, 2);
    gc::weak_ref<node> fresh = second;
    CHECK(stale.expired());
    CHECK(!stale.lock());
    CHECK_EQ(fresh.lock()->id, 2);

    // Objects created after a cycle are not candidates of an earlier one.
    d.collect_full();
    CHECK_EQ(second->id, 2);
}

namespace
{
// Destructor that tries to promote weak references to other garbage.
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

// C06 (synchronous part): once the candidate set is fixed, promotion of any
// candidate fails, including from destructors running in the same sweep.
GC_TEST(c06_no_promotion_of_condemned_objects)
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
    CHECK_EQ(r.reclaimed, std::size_t{2});
    CHECK_EQ(prober::attempts, 2);
    CHECK_EQ(prober::promoted, 0);
}

GC_TEST(weak_conversion_to_base)
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
    CHECK_EQ(wb.lock()->v, 5);
    c = nullptr;
    d.collect_full();
    CHECK(wb.expired());
}
