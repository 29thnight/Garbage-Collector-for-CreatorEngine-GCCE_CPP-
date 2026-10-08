#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using gctest::node;

namespace
{
// One unit of work per step: makes slice boundaries deterministic.
const gc::step_budget one_unit{gc::duration::zero(), 1};

// Builds root -> c1 -> c2 -> ... -> cN through `next` and returns the raw
// pointers, index 0 being the root. Raw pointers are only read, never stored.
std::vector<node*> make_chain(gc::domain& d, gc::root_ref<node>& root, int length)
{
    root = gc::make<node>(d, 0);
    std::vector<node*> chain{root.get()};
    for (int i = 1; i <= length; ++i)
    {
        chain.back()->next = gc::make<node>(d, i);
        chain.push_back(chain.back()->next.get());
    }
    return chain;
}

gc::step_result step_until(gc::domain& d, bool (*done)(const gc::step_result&))
{
    for (int guard = 0; guard < 1000000; ++guard)
    {
        auto r = d.collect_step(one_unit);
        if (done(r) || r.cycle_finished)
            return r;
    }
    gctest::fail(__FILE__, __LINE__, "cycle did not progress");
}

void finish_cycle(gc::domain& d)
{
    if (d.current_phase() == gc::phase::idle)
        return;
    step_until(d, [](const gc::step_result&) { return false; });
}
} // namespace

// C03: an already traced object gains an edge to an unvisited object whose
// only other path is then cut. The insertion barrier keeps the target.
GC_TEST(c03_black_object_gains_edge_to_unvisited)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 100);
    gc::weak_ref<node> tail = chain[99]->next;

    d.request_collection();
    auto r = d.collect_step(one_unit); // root is the only gray object: traced
    CHECK(r.cycle_started);
    CHECK(d.current_phase() == gc::phase::marking);
    const auto shades_before = d.stats().barrier_shades;

    root->children.push_back(chain[99]->next); // root is traced, tail is not
    chain[50]->next = nullptr;                 // cut the only other path
    CHECK(d.stats().barrier_shades > shades_before);

    finish_cycle(d);
    CHECK(!tail.expired());
    CHECK_EQ(d.stats().last.reclaimed, std::size_t{49}); // c51..c99
    CHECK_EQ(d.stats().live_objects, std::size_t{52});   // root, c1..c50, c100
}

// C05 / C06: the gray stack running dry does not end marking when a weak
// promotion adds work afterwards; the promoted object and its subgraph live.
GC_TEST(c05_c06_promotion_after_gray_drained)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 20);
    (void)chain;

    gc::weak_ref<node> garbage;
    gc::weak_ref<node> garbage_child;
    {
        auto g = gc::make<node>(d, 100);
        g->next = gc::make<node>(d, 101);
        garbage = g;
        garbage_child = g->next;
    }

    d.request_collection();
    // Trace the 21 reachable objects one by one; the gray stack is then empty.
    for (int i = 0; i < 21; ++i)
        CHECK(!d.collect_step(one_unit).finalized);
    CHECK_EQ(d.stats().current.marked, std::size_t{21});
    CHECK(d.current_phase() == gc::phase::marking);

    gc::root_ref<node> promoted = garbage.lock(); // legal during marking
    CHECK(promoted);

    finish_cycle(d);
    CHECK_EQ(d.stats().last.reclaimed, std::size_t{0});
    CHECK(!garbage_child.expired());
    CHECK_EQ(promoted->next->id, 101);
}

// C06: once the reclaim decision is taken, every candidate refuses
// promotion even though its storage has not been swept yet.
GC_TEST(c06_candidates_refuse_promotion_before_sweep)
{
    gc::domain d;
    auto keep = gc::make<node>(d, 0);
    std::vector<gc::weak_ref<node>> garbage;
    for (int i = 1; i <= 1000; ++i)
        garbage.push_back(gc::make<node>(d, i));

    d.request_collection();
    auto r = step_until(d, [](const gc::step_result& s) { return s.finalized; });
    CHECK(r.finalized);
    CHECK(d.current_phase() == gc::phase::sweeping);
    CHECK_EQ(d.stats().live_objects, std::size_t{1001}); // nothing swept yet
    for (const auto& w : garbage)
        CHECK(!w.lock());
    gc::weak_ref<node> survivor = keep;
    CHECK(survivor.lock());

    finish_cycle(d);
    CHECK_EQ(d.stats().live_objects, std::size_t{1});
}

// C08: objects created during sweep, including ones that reuse free slots
// the sweep cursor has not reached yet, are not candidates of the running
// cycle, and they can be promoted while the sweep is still in progress.
GC_TEST(c08_objects_created_during_sweep_survive)
{
    gc::domain d;
    auto keep = gc::make<node>(d, 0);
    for (int i = 0; i < 1000; ++i)
        (void)gc::make<node>(d, i);
    d.collect_full(); // 1000 free slots
    for (int i = 0; i < 500; ++i)
        (void)gc::make<node>(d, i); // reuse half; the rest stay free at finalize

    d.request_collection();
    step_until(d, [](const gc::step_result& s) { return s.finalized; });
    CHECK(d.current_phase() == gc::phase::sweeping);

    std::vector<gc::weak_ref<node>> created;
    while (d.current_phase() == gc::phase::sweeping)
    {
        for (int i = 0; i < 5; ++i)
        {
            created.push_back(gc::make<node>(d, -1)); // unrooted on purpose
            CHECK(created.back().lock());              // promotable during sweep
        }
        d.collect_step(one_unit);
    }
    CHECK(created.size() > 100);
    for (const auto& w : created)
        CHECK(!w.expired());
    CHECK_EQ(d.stats().last.reclaimed, std::size_t{500});

    // They are ordinary garbage for the next cycle.
    auto r = d.collect_full();
    CHECK_EQ(r.reclaimed, created.size());
}

// An obligation that appears mid-mark keeps the object for this cycle even
// if its path is cut; the next cycle reports it instead of reclaiming it.
GC_TEST(obligation_set_during_mark_is_not_missed)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 50);
    gc::weak_ref<node> target = chain[49]->next;

    d.request_collection();
    d.collect_step(one_unit);
    CHECK(gc::begin_cleanup_obligation(chain[49]->next));
    chain[10]->next = nullptr;
    finish_cycle(d);
    CHECK(!target.expired());
    CHECK_EQ(rec.total(), std::size_t{0});

    auto r = d.collect_full();
    CHECK(r.aborted);
    CHECK_EQ(rec.count(gc::violation_kind::unreachable_with_cleanup_obligation), std::size_t{1});
    CHECK(!target.expired());

    auto t = target.lock();
    CHECK(gc::advance_lifecycle(t, gc::lifecycle_state::destroy_requested));
    CHECK(gc::advance_lifecycle(t, gc::lifecycle_state::destroying));
    CHECK(gc::advance_lifecycle(t, gc::lifecycle_state::destroyed));
    t = nullptr;
    CHECK(d.collect_full().completed);
    CHECK(target.expired());
}

namespace
{
struct big
{
    std::vector<gc::trace_ref<node>> refs;
    void gc_trace(gc::tracer& t) const { t.visit(refs); }
};
} // namespace

// A single large container is one indivisible unit: the overrun is visible
// in the step result and the statistics. Reallocating it during marking runs
// the barrier once per element, which is counted.
GC_TEST(budget_overrun_and_barrier_cost_are_observable)
{
    gc::domain d;
    auto b = gc::make<big>(d);
    auto target = gc::make<node>(d, 1);
    constexpr std::size_t n = 200000;
    b->refs.assign(n, gc::trace_ref<node>(target));
    b->refs.shrink_to_fit();
    target = nullptr;

    d.request_collection();
    const gc::step_budget tiny{std::chrono::nanoseconds(1), 1};
    auto r = d.collect_step(tiny); // traces `big`: 200000 edges in one unit
    CHECK(r.over_budget);
    CHECK_EQ(r.units, std::size_t{1});
    CHECK(r.longest_unit > tiny.time);
    CHECK_EQ(d.stats().current.edges_visited, n);
    CHECK(d.stats().worst_unit >= r.longest_unit);
    CHECK(d.stats().steps_over_budget >= 1);

    const auto stores_before = d.stats().barrier_stores_during_mark;
    b->refs.push_back(b->refs.front()); // reallocates: every element is moved
    CHECK(d.stats().barrier_stores_during_mark - stores_before >= n);

    bool saw_finalize = false;
    while (d.current_phase() != gc::phase::idle)
        saw_finalize |= d.collect_step(tiny).finalized;
    CHECK(saw_finalize);
    CHECK(d.stats().last.completed);
    CHECK(d.stats().last_cycle_steps >= 2);
}

GC_TEST(step_starts_only_when_due)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);

    auto r = d.collect_step();
    CHECK(!r.cycle_started);
    CHECK_EQ(r.units, std::size_t{0});

    d.request_collection();
    r = d.collect_step({gc::duration::zero(), 1});
    CHECK(r.cycle_started);
    CHECK(!d.collection_requested()); // consumed by the cycle start
    finish_cycle(d);

    // The maximum interval starts a cycle without a request.
    d.set_max_cycle_interval(std::chrono::nanoseconds(1));
    r = d.collect_step();
    CHECK(r.cycle_started);
    finish_cycle(d);
    d.set_max_cycle_interval(gc::duration::zero());
    CHECK(!d.collect_step().cycle_started);
}

GC_TEST(minimum_progress_per_step)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> roots;
    for (int i = 0; i < 100; ++i)
        roots.push_back(gc::make<node>(d, i));
    d.request_collection();
    auto r = d.collect_step({gc::duration::zero(), 16});
    CHECK(!r.cycle_finished);
    CHECK_EQ(r.units, std::size_t{16});
}

// collect_full finishes a cycle in progress and then runs a fresh one, so
// garbage that floated through the old cycle is reclaimed too.
GC_TEST(collect_full_finishes_cycle_in_progress)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 10);
    d.request_collection();
    d.collect_step(one_unit);
    chain[5]->next = nullptr; // c6..c10 were not yet traced
    root->children.push_back(gc::make<node>(d, 99));
    root->children.clear(); // marked by the barrier: floats through this cycle

    auto r = d.collect_full();
    CHECK(r.completed);
    CHECK(d.current_phase() == gc::phase::idle);
    CHECK_EQ(d.stats().live_objects, std::size_t{6});
}

namespace
{
// Node with an address registry so the test can tell a reclaimed object
// from a live one without touching freed memory.
struct tnode
{
    static inline std::unordered_set<const tnode*> live;
    int id;
    gc::trace_ref<tnode> next;
    std::vector<gc::trace_ref<tnode>> children;
    explicit tnode(int i) : id(i) { live.insert(this); }
    ~tnode() { live.erase(this); }
    void gc_trace(gc::tracer& t) const
    {
        t.visit(next);
        t.visit(children);
    }
};

struct walk
{
    std::vector<tnode*> nodes;                 // reachable, each once
    std::vector<const gc::trace_ref<tnode>*> edges; // every non-null edge seen
    bool lost = false;                         // reached a reclaimed object
    int lost_id = -1;
};

// Raw-pointer traversal: reads only, so it never runs the barrier.
walk reachable(const std::vector<gc::root_ref<tnode>>& roots)
{
    walk w;
    std::unordered_set<const tnode*> seen;
    std::vector<tnode*> stack;
    auto push = [&](tnode* n) {
        if (!tnode::live.count(n))
        {
            w.lost = true;
            return;
        }
        if (seen.insert(n).second)
            stack.push_back(n);
    };
    for (const auto& r : roots)
        if (r)
            push(r.get());
    while (!stack.empty() && !w.lost)
    {
        tnode* n = stack.back();
        stack.pop_back();
        w.nodes.push_back(n);
        if (n->next)
        {
            w.edges.push_back(&n->next);
            push(n->next.get());
        }
        for (const auto& c : n->children)
            if (c)
            {
                w.edges.push_back(&c);
                push(c.get());
            }
    }
    return w;
}

void run_incremental_random(std::uint32_t seed)
{
    const std::string where = "seed=" + std::to_string(seed);
    std::mt19937 rng(seed);
    auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };

    gc::domain d;
    std::vector<gc::root_ref<tnode>> roots;
    std::vector<gc::weak_ref<tnode>> handles;
    int next_id = 0;
    auto create = [&]() {
        auto n = gc::make<tnode>(d, next_id++);
        handles.push_back(n);
        return n;
    };
    for (int i = 0; i < 8; ++i)
        roots.push_back(create());

    for (int round = 0; round < 600; ++round)
    {
        // A few mutations between slices.
        for (int m = 0; m < 4; ++m)
        {
            walk w = reachable(roots);
            if (w.nodes.empty())
            {
                roots.push_back(create());
                continue;
            }
            tnode* from = w.nodes[pick(w.nodes.size())];
            switch (pick(7))
            {
            case 0: // new object attached to a reachable one
                from->children.push_back(create());
                break;
            case 1: // copy an existing edge (target may be untraced)
                if (!w.edges.empty())
                    from->children.push_back(*w.edges[pick(w.edges.size())]);
                break;
            case 2:
                if (!w.edges.empty())
                    from->next = *w.edges[pick(w.edges.size())];
                break;
            case 3:
                from->next = nullptr;
                break;
            case 4: // erase, possibly from the middle
                if (!from->children.empty())
                    from->children.erase(from->children.begin() +
                                         static_cast<std::ptrdiff_t>(pick(from->children.size())));
                break;
            case 5: // drop a root
                if (roots.size() > 1)
                    roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(pick(roots.size())));
                break;
            case 6: // promote any handle, garbage included
                if (auto r = handles[pick(handles.size())].lock())
                    roots.push_back(std::move(r));
                break;
            }
        }

        if (pick(3) == 0)
            d.request_collection();
        const gc::step_budget budget{gc::duration::zero(), 1 + pick(8)};
        d.collect_step(budget);

        walk after = reachable(roots);
        if (after.lost)
            gctest::fail(__FILE__, __LINE__,
                         where + " round=" + std::to_string(round) + ": a reachable object was reclaimed");
    }

    // Stop mutating: finish the running cycle, then one full incremental
    // cycle must leave exactly the reachable set.
    finish_cycle(d);
    d.request_collection();
    d.collect_step(one_unit);
    finish_cycle(d);
    walk final_walk = reachable(roots);
    if (final_walk.lost)
        gctest::fail(__FILE__, __LINE__, where + ": reachable object lost at the end");
    if (d.stats().live_objects != final_walk.nodes.size())
        gctest::fail(__FILE__, __LINE__,
                     where + ": garbage left after a stable cycle (" + std::to_string(d.stats().live_objects) +
                         " live vs " + std::to_string(final_walk.nodes.size()) + " reachable)");

    roots.clear();
    d.collect_full();
    if (!tnode::live.empty())
        gctest::fail(__FILE__, __LINE__, where + ": objects left after dropping all roots");
}
} // namespace

// C12 (incremental): random graph mutations interleaved with slices of
// random size. No reachable object is ever reclaimed, and after mutation
// stops one stable cycle reclaims all garbage. The seed is in every message.
GC_TEST(c12_incremental_random_mutation)
{
    for (std::uint32_t seed = 1; seed <= 40; ++seed)
        run_incremental_random(seed);
}
