#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using gctest::node;

namespace
{
// A nested value type that is traced through its own gc_trace.
struct binding
{
    gc::trace_ref<node> target;
    float weight = 1.0f;
    void gc_trace(gc::tracer& t) const { t.visit(target); }
};

struct holder
{
    std::array<gc::trace_ref<node>, 2> fixed;
    std::optional<gc::trace_ref<node>> maybe;
    std::map<int, binding> by_id;
    std::unordered_map<std::string, gc::trace_ref<node>> by_name;
    std::vector<std::pair<int, gc::trace_ref<node>>> pairs;
    std::vector<std::vector<gc::trace_ref<node>>> nested;
    std::optional<binding> maybe_binding;
    gc::weak_ref<node> observer; // never traced

    void gc_trace(gc::tracer& t) const
    {
        t.visit(fixed);
        t.visit(maybe);
        t.visit(by_id);
        t.visit(by_name);
        t.visit(pairs);
        t.visit(nested);
        t.visit(maybe_binding);
    }
};
} // namespace

GC_TEST(container_adapters_trace_every_reference)
{
    gc::domain d;
    auto h = gc::make<holder>(d);
    h->fixed[0] = gc::make<node>(d, 1);
    h->fixed[1] = gc::make<node>(d, 2);
    h->maybe = gc::trace_ref<node>(gc::make<node>(d, 3));
    h->by_id[4].target = gc::make<node>(d, 4);
    h->by_name["five"] = gc::make<node>(d, 5);
    h->pairs.emplace_back(6, gc::make<node>(d, 6));
    h->nested.resize(2);
    h->nested[1].push_back(gc::make<node>(d, 7));
    h->maybe_binding = binding{gc::make<node>(d, 8), 0.5f};

    auto unrooted_observed = gc::make<node>(d, 9);
    h->observer = unrooted_observed;
    gc::weak_ref<node> observed = unrooted_observed;
    unrooted_observed = nullptr;

    auto r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{1}); // only the weakly observed node
    CHECK(observed.expired());
    CHECK_EQ(d.stats().live_objects, std::size_t{9});
    CHECK_EQ(h->nested[1][0]->id, 7);
    CHECK_EQ(h->maybe_binding->target->id, 8);

    // Removing entries drops exactly those edges.
    h->by_id.clear();
    h->nested.clear();
    r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{2});

    h = nullptr;
    r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{7});
    CHECK_EQ(d.stats().live_objects, std::size_t{0});
}

// C04 (synchronous part): erase, reorder, swap and reallocation of a traced
// container between cycles never lose a surviving edge.
GC_TEST(c04_container_mutation_between_cycles)
{
    gc::domain d;
    auto owner = gc::make<node>(d, 0);
    for (int i = 1; i <= 64; ++i)
        owner->children.push_back(gc::make<node>(d, i));

    // swap-erase every third element
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

    auto r = d.collect_full();
    CHECK_EQ(r.reclaimed, std::size_t{21});
    for (const auto& c : owner->children)
        CHECK(c->id % 3 != 0);
    CHECK_EQ(d.stats().live_objects, owner->children.size() + 1);
}
