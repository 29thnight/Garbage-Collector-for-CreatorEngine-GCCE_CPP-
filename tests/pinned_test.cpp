#include "support.hpp"

#include <atomic>
#include <thread>
#include <vector>

using gctest::live;
using gctest::node;

TEST(Pinned, KeepsTheObjectAliveForWorkers)
{
    gc::domain d;
    gc::weak_ref<node> watch;
    gc::pinned<node> pin;
    {
        auto n = gc::make<node>(d, 7);
        for (int i = 0; i < 100; ++i)
            n->children.push_back(gc::make<node>(d, i));
        watch = n;
        pin = gc::pinned<node>(n);
    }
    // Only the pin keeps the graph alive while the worker reads it.
    std::atomic<long> sum{0};
    std::thread worker([p = pin.get(), &sum] {
        long local = 0;
        for (const auto& c : p->children)
            local += c->id;
        sum = local;
    });
    d.collect_full();
    worker.join();
    EXPECT_EQ(sum.load(), 4950);
    EXPECT_FALSE(watch.expired());

    pin.release();
    d.collect_full();
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(live(d), 0u);
}

TEST(Pinned, IsLabelledAndMoveOnly)
{
    static_assert(!std::is_copy_constructible_v<gc::pinned<node>>);
    static_assert(std::is_move_constructible_v<gc::pinned<node>>);

    gc::domain d;
    auto n = gc::make<node>(d, 1);
    gc::pinned<node> a(n);
    gc::pinned<node> b(std::move(a));
    EXPECT_FALSE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->id, 1);

    std::size_t pinned_roots = 0;
    d.for_each_root([&](const gc::object_info& info) {
        if (info.root_label && std::string(info.root_label) == "gc.pinned")
            ++pinned_roots;
    });
    EXPECT_EQ(pinned_roots, 1u);
}

TEST(Pinned, FromAnEdge)
{
    gc::domain d;
    auto owner = gc::make<node>(d, 0);
    owner->next = gc::make<node>(d, 1);
    gc::pinned<node> pin(owner->next);
    owner->next = nullptr;
    d.collect_full();
    EXPECT_EQ(pin->id, 1);
}
