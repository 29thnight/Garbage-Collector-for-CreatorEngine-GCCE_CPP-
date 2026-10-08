#include "support.hpp"

#include <chrono>
#include <vector>

using gctest::finish_cycle;
using gctest::live;
using gctest::make_chain;
using gctest::node;
using gctest::one_unit;
using gctest::step_until_finalized;

// C03: an object that was already traced gains an edge to an untraced
// object whose only other path is then cut. The insertion barrier keeps it.
TEST(Incremental, TracedObjectGainsEdgeToUntracedObject)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 100);
    gc::weak_ref<node> tail = chain[99]->next;

    d.request_collection();
    auto r = d.collect_step(one_unit); // the root is the only gray object
    ASSERT_TRUE(r.cycle_started);
    ASSERT_EQ(d.current_phase(), gc::phase::marking);
    const auto shades_before = d.stats().barrier_shades;

    root->children.push_back(chain[99]->next);
    chain[50]->next = nullptr;
    EXPECT_GT(d.stats().barrier_shades, shades_before);

    finish_cycle(d);
    EXPECT_FALSE(tail.expired());
    EXPECT_EQ(d.stats().last.reclaimed, 49u); // c51..c99
    EXPECT_EQ(live(d), 52u);
}

// C02 during marking: a root created mid-cycle protects an untraced object
// whose path is then cut.
TEST(Incremental, RootCreatedDuringMarking)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 30);
    d.request_collection();
    d.collect_step(one_unit);

    gc::root_ref<node> late = chain[29]->next;
    chain[5]->next = nullptr;
    finish_cycle(d);
    EXPECT_EQ(late->id, 30);
    EXPECT_EQ(live(d), 7u); // root, c1..c5, c30
}

// C05 / C06: the gray stack running dry does not end marking when a weak
// promotion adds work afterwards; the promoted object and its subgraph live.
TEST(Incremental, PromotionAfterGrayStackRanDry)
{
    gc::domain d;
    gc::root_ref<node> root;
    make_chain(d, root, 20);

    gc::weak_ref<node> garbage;
    gc::weak_ref<node> garbage_child;
    {
        auto g = gc::make<node>(d, 100);
        g->next = gc::make<node>(d, 101);
        garbage = g;
        garbage_child = g->next;
    }

    d.request_collection();
    for (int i = 0; i < 21; ++i)
        ASSERT_FALSE(d.collect_step(one_unit).finalized);
    ASSERT_EQ(d.stats().current.marked, 21u);
    ASSERT_EQ(d.current_phase(), gc::phase::marking);

    gc::root_ref<node> promoted = garbage.lock();
    ASSERT_TRUE(promoted);

    finish_cycle(d);
    EXPECT_EQ(d.stats().last.reclaimed, 0u);
    EXPECT_FALSE(garbage_child.expired());
    EXPECT_EQ(promoted->next->id, 101);
}

// C06: after the reclaim decision every candidate refuses promotion even
// though its storage has not been swept yet.
TEST(Incremental, CandidatesRefusePromotionBeforeSweep)
{
    gc::domain d;
    auto keep = gc::make<node>(d, 0);
    std::vector<gc::weak_ref<node>> garbage;
    for (int i = 1; i <= 1000; ++i)
        garbage.push_back(gc::make<node>(d, i));

    step_until_finalized(d);
    ASSERT_EQ(d.current_phase(), gc::phase::sweeping);
    EXPECT_EQ(live(d), 1001u); // nothing swept yet
    for (const auto& w : garbage)
        EXPECT_FALSE(w.lock());
    EXPECT_TRUE(gc::weak_ref<node>(keep).lock());

    finish_cycle(d);
    EXPECT_EQ(live(d), 1u);
}

// C08: objects created during sweep, including ones in free slots the sweep
// cursor has not reached, are not candidates and can be promoted.
TEST(Incremental, ObjectsCreatedDuringSweepSurvive)
{
    gc::domain d;
    auto keep = gc::make<node>(d, 0);
    for (int i = 0; i < 1000; ++i)
        (void)gc::make<node>(d, i);
    d.collect_full(); // 1000 free slots
    for (int i = 0; i < 500; ++i)
        (void)gc::make<node>(d, i); // half reused, half still free

    step_until_finalized(d);
    ASSERT_EQ(d.current_phase(), gc::phase::sweeping);

    std::vector<gc::weak_ref<node>> created;
    while (d.current_phase() == gc::phase::sweeping)
    {
        for (int i = 0; i < 5; ++i)
        {
            created.push_back(gc::make<node>(d, -1)); // unrooted on purpose
            EXPECT_TRUE(created.back().lock());
        }
        d.collect_step(one_unit);
    }
    EXPECT_GT(created.size(), 100u);
    for (const auto& w : created)
        EXPECT_FALSE(w.expired());
    EXPECT_EQ(d.stats().last.reclaimed, 500u);

    EXPECT_EQ(d.collect_full().reclaimed, created.size());
}

// An obligation that appears mid-mark keeps the object for this cycle even
// if its path is cut; the next cycle reports it instead of reclaiming it.
TEST(Incremental, ObligationSetDuringMarkingIsNotMissed)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 50);
    gc::weak_ref<node> target = chain[49]->next;

    d.request_collection();
    d.collect_step(one_unit);
    ASSERT_TRUE(gc::begin_cleanup_obligation(chain[49]->next));
    chain[10]->next = nullptr;
    finish_cycle(d);
    EXPECT_FALSE(target.expired());
    EXPECT_EQ(rec.total(), 0u);

    EXPECT_TRUE(d.collect_full().aborted);
    EXPECT_EQ(rec.count(gc::violation_kind::unreachable_with_cleanup_obligation), 1u);
    EXPECT_FALSE(target.expired());
}

namespace
{
struct big : gc::managed
{
    std::vector<gc::trace_ref<node>> refs;
    void gc_trace(gc::tracer& t) const { t.visit(refs); }
};
} // namespace

// One large container is one indivisible unit; the overrun is visible.
// Reallocating it during marking runs the barrier once per element.
TEST(Incremental, BudgetOverrunAndBarrierCostAreObservable)
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
    auto r = d.collect_step(tiny);
    EXPECT_TRUE(r.over_budget);
    EXPECT_EQ(r.units, 1u);
    EXPECT_GT(r.longest_unit, tiny.time);
    EXPECT_EQ(d.stats().current.edges_visited, n);
    EXPECT_GE(d.stats().worst_unit, r.longest_unit);
    EXPECT_GE(d.stats().steps_over_budget, 1u);

    const auto stores_before = d.stats().barrier_stores_during_mark;
    b->refs.push_back(b->refs.front()); // reallocates
    EXPECT_GE(d.stats().barrier_stores_during_mark - stores_before, n);

    bool saw_finalize = false;
    while (d.current_phase() != gc::phase::idle)
        saw_finalize |= d.collect_step(tiny).finalized;
    EXPECT_TRUE(saw_finalize);
    EXPECT_TRUE(d.stats().last.completed);
    EXPECT_GE(d.stats().last_cycle_steps, 2u);
}

TEST(Incremental, StepStartsOnlyWhenDue)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);

    auto r = d.collect_step();
    EXPECT_FALSE(r.cycle_started);
    EXPECT_EQ(r.units, 0u);

    d.request_collection();
    r = d.collect_step(one_unit);
    EXPECT_TRUE(r.cycle_started);
    EXPECT_FALSE(d.collection_requested()); // consumed by the cycle start
    finish_cycle(d);

    d.set_max_cycle_interval(std::chrono::nanoseconds(1));
    EXPECT_TRUE(d.collect_step().cycle_started);
    finish_cycle(d);
    d.set_max_cycle_interval(gc::duration::zero());
    EXPECT_FALSE(d.collect_step().cycle_started);
}

TEST(Incremental, MinimumProgressPerStep)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> roots;
    for (int i = 0; i < 100; ++i)
        roots.push_back(gc::make<node>(d, i));
    d.request_collection();
    auto r = d.collect_step({gc::duration::zero(), 16});
    EXPECT_FALSE(r.cycle_finished);
    EXPECT_EQ(r.units, 16u);
}

// collect_full finishes a cycle in progress, then runs a fresh one, so
// garbage that floated through the old cycle is reclaimed as well.
TEST(Incremental, CollectFullFinishesCycleInProgress)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 10);
    d.request_collection();
    d.collect_step(one_unit);
    chain[5]->next = nullptr;
    root->children.push_back(gc::make<node>(d, 99));
    root->children.clear(); // marked by the barrier: floating garbage

    EXPECT_TRUE(d.collect_full().completed);
    EXPECT_EQ(d.current_phase(), gc::phase::idle);
    EXPECT_EQ(live(d), 6u);
}

TEST(Incremental, StepFromDestructorIsRefused)
{
    struct steps_in_destructor : gc::managed
    {
        gc::domain* d;
        explicit steps_in_destructor(gc::domain* domain) : d(domain) {}
        ~steps_in_destructor() { EXPECT_TRUE(d->collect_step().refused); }
    };

    gc::domain d;
    gctest::violation_recorder rec(d);
    (void)gc::make<steps_in_destructor>(d, &d);
    d.collect_full();
    EXPECT_EQ(rec.count(gc::violation_kind::nested_collection), 1u);
}
