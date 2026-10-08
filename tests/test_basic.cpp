#include "test_framework.hpp"

#include <utility>
#include <vector>

using gctest::node;

// C01: chain, self cycle and mutual cycle are kept while rooted and reclaimed
// once the root is gone.
GC_TEST(c01_chain_and_cycles)
{
    gc::domain d;
    {
        auto head = gc::make<node>(d, 1);
        head->next = gc::make<node>(d, 2);
        head->next->next = gc::make<node>(d, 3);

        auto self = gc::make<node>(d, 10);
        self->next = self;

        auto a = gc::make<node>(d, 20);
        auto b = gc::make<node>(d, 21);
        a->next = b;
        b->next = a;
        b = nullptr;

        auto r = d.collect_full();
        CHECK(r.completed);
        CHECK_EQ(r.reclaimed, std::size_t{0});
        CHECK_EQ(d.stats().live_objects, std::size_t{6});
        CHECK_EQ(head->next->next->id, 3);
        CHECK_EQ(a->next->next->id, 20);

        head = nullptr;
        r = d.collect_full();
        CHECK_EQ(r.reclaimed, std::size_t{3});
        CHECK_EQ(d.stats().live_objects, std::size_t{3});
    }
    // self and the a/b cycle lost their roots at scope exit.
    auto r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{3});
    CHECK_EQ(d.stats().live_objects, std::size_t{0});
    CHECK_EQ(node::alive, 0);
}

// C02: root copy, move, container reallocation and late roots leave no
// unprotected target and no stale registration.
GC_TEST(c02_root_copy_move_and_relocation)
{
    gc::domain d;
    {
        auto a = gc::make<node>(d, 1);
        CHECK_EQ(d.stats().roots, std::size_t{1});

        gc::root_ref<node> copy = a;
        CHECK_EQ(d.stats().roots, std::size_t{2});

        gc::root_ref<node> moved = std::move(a);
        CHECK(!a);
        CHECK_EQ(d.stats().roots, std::size_t{2});

        copy = nullptr;
        CHECK_EQ(d.stats().roots, std::size_t{1});

        // Reallocating a vector of roots moves every element.
        std::vector<gc::root_ref<node>> roots;
        for (int i = 0; i < 100; ++i)
            roots.push_back(gc::make<node>(d, 100 + i));
        CHECK_EQ(d.stats().roots, std::size_t{101});

        d.collect_full();
        CHECK_EQ(d.stats().live_objects, std::size_t{101});
        for (int i = 0; i < 100; ++i)
            CHECK_EQ(roots[static_cast<std::size_t>(i)]->id, 100 + i);

        // Erase from the middle shifts elements by move assignment.
        roots.erase(roots.begin(), roots.begin() + 50);
        CHECK_EQ(d.stats().roots, std::size_t{51});
        d.collect_full();
        CHECK_EQ(d.stats().live_objects, std::size_t{51});
        CHECK_EQ(roots.front()->id, 150);

        // A root created after a cycle protects the target in the next one.
        moved->next = gc::make<node>(d, 7);
        gc::root_ref<node> late = moved->next;
        moved->next = nullptr;
        d.collect_full();
        CHECK_EQ(late->id, 7);

        // Self assignment and retargeting keep the registration count exact.
        const auto& alias = late;
        late = alias;
        late = moved;
        CHECK(gc::same_object(late, moved));
        CHECK_EQ(d.stats().roots, std::size_t{52});
    }
    CHECK_EQ(d.stats().roots, std::size_t{0});
    d.collect_full();
    CHECK_EQ(d.stats().live_objects, std::size_t{0});
}

namespace
{
struct base_a
{
    gc::trace_ref<node> ra;
    int a = 1;
    virtual ~base_a() = default;
    void gc_trace(gc::tracer& t) const { t.visit(ra); }
};

struct base_b
{
    gc::trace_ref<node> rb;
    int b = 2;
    virtual ~base_b() = default;
    void gc_trace(gc::tracer& t) const { t.visit(rb); }
};

struct derived : base_a, base_b
{
    static inline int destroyed = 0;
    gc::trace_ref<node> rd;
    ~derived() override { ++destroyed; }
    void gc_trace(gc::tracer& t) const
    {
        base_a::gc_trace(t);
        base_b::gc_trace(t);
        t.visit(rd);
    }
};
} // namespace

// Multiple inheritance: references through a non-primary base keep the whole
// object, and tracing and destruction use the most derived type.
GC_TEST(inheritance_and_base_adjustment)
{
    gc::domain d;
    derived::destroyed = 0;
    {
        auto obj = gc::make<derived>(d);
        obj->ra = gc::make<node>(d, 1);
        obj->rb = gc::make<node>(d, 2);
        obj->rd = gc::make<node>(d, 3);

        gc::root_ref<base_b> as_b = obj;
        CHECK(as_b.get() == static_cast<base_b*>(obj.get()));
        CHECK(gc::same_object(as_b, obj));
        CHECK_EQ(as_b->b, 2);

        gc::weak_ref<base_b> weak_b = obj;
        obj = nullptr;

        auto r = d.collect_full();
        CHECK_EQ(r.reclaimed, std::size_t{0});
        CHECK_EQ(d.stats().live_objects, std::size_t{4});

        auto locked = weak_b.lock();
        CHECK(locked.get() == as_b.get());
        CHECK_EQ(locked->rb->id, 2);
    }
    auto r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{4});
    CHECK_EQ(derived::destroyed, 1);
}

// Allocation volume records a request; it never collects by itself.
GC_TEST(allocation_threshold_requests_collection)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    keep.push_back(gc::make<node>(d, 0));
    const std::size_t block = d.stats().bytes_since_last_cycle; // header + object
    d.set_allocation_threshold(block * 20);
    for (int i = 1; i < 10; ++i)
        keep.push_back(gc::make<node>(d, i));
    CHECK(!d.collection_requested());
    for (int i = 0; i < 10; ++i)
        (void)gc::make<node>(d, i);
    CHECK(d.collection_requested());
    CHECK_EQ(d.stats().cycles_completed, std::uint64_t{0});

    auto r = d.collect_full();
    CHECK(r.completed);
    CHECK_EQ(r.reclaimed, std::size_t{10});
    CHECK(!d.collection_requested());
    CHECK_EQ(d.stats().bytes_since_last_cycle, std::size_t{0});

    d.request_collection();
    CHECK(d.collection_requested());
}

// Edge and mark counters reflect the traced graph.
GC_TEST(statistics_counts)
{
    gc::domain d;
    auto root = gc::make<node>(d, 0);
    for (int i = 1; i <= 5; ++i)
        root->children.push_back(gc::make<node>(d, i));
    root->next = root->children.front();

    auto r = d.collect_full();
    CHECK_EQ(r.marked, std::size_t{6});
    CHECK_EQ(r.edges_visited, std::size_t{6});
    auto st = d.stats();
    CHECK_EQ(st.cycles_completed, std::uint64_t{1});
    CHECK_EQ(st.roots, std::size_t{1});
    CHECK(st.live_bytes >= 6 * sizeof(node));
    CHECK(st.current_phase == gc::phase::idle);
}
