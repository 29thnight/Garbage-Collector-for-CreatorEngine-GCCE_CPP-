#include "support.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using gctest::live;
using gctest::node;

// C01: chains, self cycles and mutual cycles are kept while rooted and
// reclaimed once the root is gone.
TEST(Collection, ChainsAndCycles)
{
    gc::domain d;
    {
        auto head = gc::make<node>(d, 1);
        head->next = gc::make<node>(d, 2);
        head->next->next = gc::make<node>(d, 3);

        auto self = gc::make<node>(d, 10);
        self->next = self;

        auto a = gc::make<node>(d, 20);
        a->next = gc::make<node>(d, 21);
        a->next->next = a;

        EXPECT_EQ(d.collect_full().reclaimed, 0u);
        EXPECT_EQ(live(d), 6u);

        head = nullptr;
        EXPECT_EQ(d.collect_full().reclaimed, 3u);
    }
    EXPECT_EQ(d.collect_full().reclaimed, 3u);
    EXPECT_EQ(live(d), 0u);
}

TEST(Collection, SharedNodeLivesUntilLastPathIsGone)
{
    gc::domain d;
    auto left = gc::make<node>(d, 1);
    auto right = gc::make<node>(d, 2);
    left->next = gc::make<node>(d, 3);
    right->next = left->next;

    left = nullptr;
    d.collect_full();
    EXPECT_EQ(right->next->id, 3);

    right->next = nullptr;
    d.collect_full();
    EXPECT_EQ(live(d), 1u);
}

// Marking is iterative, so very deep structures do not recurse.
TEST(Collection, DeepListDoesNotOverflowTheStack)
{
    gc::domain d;
    auto head = gc::make<node>(d, 0);
    node* tail = head.get();
    for (int i = 1; i < 200000; ++i)
    {
        tail->next = gc::make<node>(d, i);
        tail = tail->next.get();
    }
    auto r = d.collect_full();
    EXPECT_EQ(r.marked, 200000u);
    EXPECT_EQ(r.reclaimed, 0u);

    head = nullptr;
    EXPECT_EQ(d.collect_full().reclaimed, 200000u);
}

namespace
{
struct shape
{
    static inline int destroyed = 0;
    gc::trace_ref<node> tag;
    virtual ~shape() { ++destroyed; }
    virtual double area() const = 0;
    void gc_trace(gc::tracer& t) const { t.visit(tag); }
};

struct circle : shape
{
    double r;
    explicit circle(double r_) : r(r_) {}
    double area() const override { return 3.0 * r * r; }
};

struct group : shape
{
    std::vector<gc::trace_ref<shape>> items;
    double area() const override
    {
        double sum = 0;
        for (const auto& s : items)
            sum += s->area();
        return sum;
    }
    void gc_trace(gc::tracer& t) const
    {
        shape::gc_trace(t);
        t.visit(items);
    }
};
} // namespace

// Tracing and destruction use the most derived type, whatever reference
// type the object is reached through.
TEST(Collection, PolymorphicHierarchy)
{
    gc::domain d;
    shape::destroyed = 0;
    {
        auto g = gc::make<group>(d);
        g->tag = gc::make<node>(d, 7);
        g->items.push_back(gc::make<circle>(d, 1.0));
        auto inner = gc::make<group>(d);
        inner->items.push_back(gc::make<circle>(d, 2.0));
        g->items.push_back(inner);

        gc::root_ref<shape> as_base = g;
        g = nullptr;
        d.collect_full();
        EXPECT_EQ(live(d), 5u);
        EXPECT_DOUBLE_EQ(as_base->area(), 15.0);
    }
    d.collect_full();
    EXPECT_EQ(shape::destroyed, 4);
    EXPECT_EQ(live(d), 0u);
}

namespace
{
struct vbase
{
    gc::trace_ref<node> shared;
    virtual ~vbase() = default;
};
struct va : virtual vbase
{
};
struct vb : virtual vbase
{
};
struct diamond : va, vb
{
    void gc_trace(gc::tracer& t) const { t.visit(shared); }
};
} // namespace

TEST(Collection, VirtualInheritance)
{
    gc::domain d;
    auto obj = gc::make<diamond>(d);
    obj->shared = gc::make<node>(d, 1);
    gc::root_ref<vbase> base = obj;
    obj = nullptr;
    d.collect_full();
    EXPECT_EQ(live(d), 2u);
    EXPECT_EQ(base->shared->id, 1);
}

namespace
{
struct alignas(64) cache_line
{
    unsigned char data[64];
};
struct alignas(256) over_aligned
{
    int v = 3;
};
struct empty
{
};
} // namespace

TEST(Collection, AlignmentAndSize)
{
    gc::domain d;
    std::vector<gc::root_ref<cache_line>> lines;
    for (int i = 0; i < 64; ++i)
        lines.push_back(gc::make<cache_line>(d));
    for (const auto& l : lines)
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(l.get()) % 64, 0u);

    auto big = gc::make<over_aligned>(d);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(big.get()) % 256, 0u);
    EXPECT_EQ(big->v, 3);

    auto e1 = gc::make<empty>(d);
    auto e2 = gc::make<empty>(d);
    EXPECT_NE(e1.get(), e2.get());
}

namespace
{
struct resource_counter
{
    static inline int open = 0;
    resource_counter() { ++open; }
    ~resource_counter() { --open; }
};

// Non-GC members are owned normally and released by the C++ destructor
// when the object is reclaimed.
struct holder
{
    std::string name = std::string(100, 'x');
    std::vector<int> values = std::vector<int>(1000, 1);
    std::unique_ptr<resource_counter> resource = std::make_unique<resource_counter>();
    std::shared_ptr<resource_counter> shared;
};
} // namespace

TEST(Collection, ReclaimRunsDestructorsOfOrdinaryMembers)
{
    gc::domain d;
    auto shared = std::make_shared<resource_counter>();
    {
        auto h = gc::make<holder>(d);
        h->shared = shared;
        EXPECT_EQ(resource_counter::open, 2);
        EXPECT_EQ(shared.use_count(), 2);
    }
    d.collect_full();
    EXPECT_EQ(resource_counter::open, 1);
    EXPECT_EQ(shared.use_count(), 1);
}

TEST(Collection, DestructorRunsExactlyOnce)
{
    struct counted
    {
        int* counter;
        ~counted() { ++*counter; }
    };

    gc::domain d;
    int destroyed = 0;
    for (int i = 0; i < 100; ++i)
        (void)gc::make<counted>(d, &destroyed);
    d.collect_full();
    d.collect_full();
    EXPECT_EQ(destroyed, 100);
}

TEST(Collection, StatisticsCountMarkedObjectsAndEdges)
{
    gc::domain d;
    auto root = gc::make<node>(d, 0);
    for (int i = 1; i <= 5; ++i)
        root->children.push_back(gc::make<node>(d, i));
    root->next = root->children.front();

    auto r = d.collect_full();
    EXPECT_TRUE(r.completed);
    EXPECT_EQ(r.marked, 6u);
    EXPECT_EQ(r.edges_visited, 6u);
    auto st = d.stats();
    EXPECT_EQ(st.cycles_completed, 1u);
    EXPECT_EQ(st.roots, 1u);
    EXPECT_GE(st.live_bytes, 6 * sizeof(node));
    EXPECT_GE(st.peak_live_bytes, st.live_bytes);
    EXPECT_LE(st.last.mark_time + st.last.sweep_time, st.last_cycle_wall);
}

TEST(Collection, AllocationThresholdRequestsACycle)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    keep.push_back(gc::make<node>(d, 0));
    const std::size_t block = d.stats().bytes_since_last_cycle; // header + object
    d.set_allocation_threshold(block * 20);
    for (int i = 1; i < 10; ++i)
        keep.push_back(gc::make<node>(d, i));
    EXPECT_FALSE(d.collection_requested());
    for (int i = 0; i < 10; ++i)
        (void)gc::make<node>(d, i);
    EXPECT_TRUE(d.collection_requested());
    EXPECT_EQ(d.stats().cycles_completed, 0u); // a request never collects by itself

    auto r = d.collect_full();
    EXPECT_EQ(r.reclaimed, 10u);
    EXPECT_FALSE(d.collection_requested());
}
