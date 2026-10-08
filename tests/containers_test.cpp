#include "support.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

using gctest::live;
using gctest::node;

// Compile-time contract of tracer::visit.
static_assert(gc::detail::traceable<gc::trace_ref<node>>());
static_assert(gc::detail::traceable<std::vector<std::optional<gc::trace_ref<node>>>>());
static_assert(gc::detail::traceable<std::map<int, gc::trace_ref<node>>>());
static_assert(gc::detail::traceable<std::variant<int, gc::trace_ref<node>>>());
static_assert(!gc::detail::traceable<std::string>());
static_assert(!gc::detail::traceable<std::vector<int>>());
static_assert(!gc::detail::traceable<gc::weak_ref<node>>());
static_assert(!gc::detail::traceable<std::variant<int, float>>());

namespace
{
// A value type traced through its own gc_trace.
struct binding
{
    gc::trace_ref<node> target;
    float weight = 1.0f;
    void gc_trace(gc::tracer& t) const { t.visit(target); }
};

// A non-GC implementation object owned through unique_ptr.
struct impl_data
{
    std::vector<gc::trace_ref<node>> items;
    void gc_trace(gc::tracer& t) const { t.visit(items); }
};

struct everything
{
    std::array<gc::trace_ref<node>, 2> fixed;
    std::deque<gc::trace_ref<node>> queue;
    std::list<gc::trace_ref<node>> linked;
    std::optional<gc::trace_ref<node>> maybe;
    std::map<int, binding> by_id;
    std::unordered_map<std::string, gc::trace_ref<node>> by_name;
    std::unordered_set<gc::trace_ref<node>> set;
    std::vector<std::pair<int, gc::trace_ref<node>>> pairs;
    std::vector<std::vector<gc::trace_ref<node>>> nested;
    std::variant<std::monostate, int, gc::trace_ref<node>> choice;
    std::unique_ptr<impl_data> pimpl = std::make_unique<impl_data>();
    gc::weak_ref<node> observer; // never traced

    void gc_trace(gc::tracer& t) const
    {
        t.visit(fixed);
        t.visit(queue);
        t.visit(linked);
        t.visit(maybe);
        t.visit(by_id);
        t.visit(by_name);
        t.visit(set);
        t.visit(pairs);
        t.visit(nested);
        t.visit(choice);
        t.visit(pimpl);
    }
};
} // namespace

TEST(Containers, EveryAdapterKeepsItsTargets)
{
    gc::domain d;
    auto e = gc::make<everything>(d);
    int id = 0;
    auto fresh = [&] { return gc::make<node>(d, ++id); };

    e->fixed[0] = fresh();
    e->fixed[1] = fresh();
    e->queue.push_back(fresh());
    e->linked.push_back(fresh());
    e->maybe = gc::trace_ref<node>(fresh());
    e->by_id[1].target = fresh();
    e->by_name["n"] = fresh();
    e->set.insert(gc::trace_ref<node>(fresh()));
    e->pairs.emplace_back(1, fresh());
    e->nested.resize(3);
    e->nested[2].push_back(fresh());
    e->choice = gc::trace_ref<node>(fresh());
    e->pimpl->items.push_back(fresh());
    const int expected = id; // 12 nodes

    auto observed = fresh();
    e->observer = observed;
    gc::weak_ref<node> watch = observed;
    observed = nullptr;

    auto r = d.collect_full();
    EXPECT_EQ(r.reclaimed, 1u); // only the weakly observed node
    EXPECT_TRUE(watch.expired());
    EXPECT_EQ(live(d), static_cast<std::size_t>(expected) + 1);

    e->choice = 42; // the variant no longer holds a reference
    e->pimpl.reset();
    e->set.clear();
    EXPECT_EQ(d.collect_full().reclaimed, 3u);

    e = nullptr;
    d.collect_full();
    EXPECT_EQ(live(d), 0u);
}

// C04: erase, swap-erase, reorder and reallocation between cycles never lose
// a surviving edge.
TEST(Containers, MutationBetweenCycles)
{
    gc::domain d;
    auto owner = gc::make<node>(d, 0);
    for (int i = 1; i <= 64; ++i)
        owner->children.push_back(gc::make<node>(d, i));

    for (std::size_t i = 0; i < owner->children.size();)
    {
        if (owner->children[i]->id % 3 == 0)
        {
            std::swap(owner->children[i], owner->children.back());
            owner->children.pop_back();
        }
        else
            ++i;
    }
    std::reverse(owner->children.begin(), owner->children.end());
    owner->children.shrink_to_fit();

    EXPECT_EQ(d.collect_full().reclaimed, 21u);
    for (const auto& c : owner->children)
        EXPECT_NE(c->id % 3, 0);
    EXPECT_EQ(live(d), owner->children.size() + 1);
}

TEST(Containers, SortingAndAlgorithmsOnReferences)
{
    gc::domain d;
    auto owner = gc::make<node>(d, 0);
    for (int i : {5, 3, 9, 1, 7})
        owner->children.push_back(gc::make<node>(d, i));

    std::sort(owner->children.begin(), owner->children.end(),
              [](const auto& a, const auto& b) { return a->id < b->id; });
    owner->children.erase(std::remove_if(owner->children.begin(), owner->children.end(),
                                         [](const auto& n) { return n->id > 6; }),
                          owner->children.end());

    d.collect_full();
    ASSERT_EQ(owner->children.size(), 3u);
    EXPECT_EQ(owner->children[0]->id, 1);
    EXPECT_EQ(owner->children[2]->id, 5);
    EXPECT_EQ(live(d), 4u);
}
