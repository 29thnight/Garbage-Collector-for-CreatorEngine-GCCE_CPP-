// C12: random graphs and mutations checked against an independent
// reachability oracle. Each case is parameterized by its seed, so a failure
// names the seed that reproduces it.

#include "support.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using gctest::finish_cycle;
using gctest::live;
using gctest::one_unit;

namespace
{
// Node with an address registry so a reclaimed object can be told from a
// live one without touching freed memory.
struct tnode : gc::managed
{
    static inline std::unordered_set<const tnode*> registry;
    int id;
    gc::trace_ref<tnode> next;
    std::vector<gc::trace_ref<tnode>> children;
    explicit tnode(int i) : id(i) { registry.insert(this); }
    ~tnode() { registry.erase(this); }
    void gc_trace(gc::tracer& t) const
    {
        t.visit(next);
        t.visit(children);
    }
};

struct walk
{
    std::vector<tnode*> nodes;                      // reachable, each once
    std::vector<const gc::trace_ref<tnode>*> edges; // every non-null edge seen
    bool lost = false;                              // reached a reclaimed object
};

// Raw-pointer traversal: reads only, never runs the barrier.
walk reachable(const std::vector<gc::root_ref<tnode>>& roots)
{
    walk w;
    std::unordered_set<const tnode*> seen;
    std::vector<tnode*> stack;
    auto push = [&](tnode* n) {
        if (!tnode::registry.count(n))
            w.lost = true;
        else if (seen.insert(n).second)
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

class mutator
{
public:
    mutator(gc::domain& d, std::uint32_t seed) : d_(d), rng_(seed)
    {
        for (int i = 0; i < 8; ++i)
            roots_.push_back(create());
    }

    std::size_t pick(std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng_); }

    void mutate(int count)
    {
        for (int m = 0; m < count; ++m)
        {
            walk w = reachable(roots_);
            if (w.nodes.empty())
            {
                roots_.push_back(create());
                continue;
            }
            tnode* from = w.nodes[pick(w.nodes.size())];
            switch (pick(7))
            {
            case 0: from->children.push_back(create()); break;
            case 1: // copy an existing edge; its target may not be traced yet
                if (!w.edges.empty())
                    from->children.push_back(*w.edges[pick(w.edges.size())]);
                break;
            case 2:
                if (!w.edges.empty())
                    from->next = *w.edges[pick(w.edges.size())];
                break;
            case 3: from->next = nullptr; break;
            case 4:
                if (!from->children.empty())
                    from->children.erase(from->children.begin() +
                                         static_cast<std::ptrdiff_t>(pick(from->children.size())));
                break;
            case 5:
                if (roots_.size() > 1)
                    roots_.erase(roots_.begin() + static_cast<std::ptrdiff_t>(pick(roots_.size())));
                break;
            case 6: // promote any handle, garbage included
                if (auto r = handles_[pick(handles_.size())].lock())
                    roots_.push_back(std::move(r));
                break;
            }
        }
    }

    std::vector<gc::root_ref<tnode>>& roots() { return roots_; }

private:
    gc::root_ref<tnode> create()
    {
        auto n = gc::make<tnode>(d_, next_id_++);
        handles_.push_back(n);
        return n;
    }

    gc::domain& d_;
    std::mt19937 rng_;
    std::vector<gc::root_ref<tnode>> roots_;
    std::vector<gc::weak_ref<tnode>> handles_;
    int next_id_ = 0;
};

class RandomGraph : public ::testing::TestWithParam<std::uint32_t>
{
};
} // namespace

// Stop-the-world baseline: after every full cycle the live set equals the
// oracle's reachable set exactly.
TEST_P(RandomGraph, FullCollectionMatchesOracle)
{
    gc::domain d;
    mutator m(d, GetParam());
    for (int round = 0; round < 40; ++round)
    {
        m.mutate(25);
        d.collect_full();
        walk w = reachable(m.roots());
        ASSERT_FALSE(w.lost) << "round " << round;
        ASSERT_EQ(live(d), w.nodes.size()) << "round " << round;
    }
    m.roots().clear();
    d.collect_full();
    EXPECT_TRUE(tnode::registry.empty());
}

// Incremental: mutations between slices of random size. A reachable object
// is never reclaimed; after mutation stops, one stable cycle leaves exactly
// the reachable set.
TEST_P(RandomGraph, IncrementalCollectionNeverLosesReachableObjects)
{
    gc::domain d;
    mutator m(d, GetParam());
    for (int round = 0; round < 600; ++round)
    {
        m.mutate(4);
        if (m.pick(3) == 0)
            d.request_collection();
        d.collect_step({gc::duration::zero(), 1 + m.pick(8)});
        ASSERT_FALSE(reachable(m.roots()).lost) << "round " << round;
    }

    finish_cycle(d);
    d.request_collection();
    d.collect_step(one_unit);
    finish_cycle(d);
    walk w = reachable(m.roots());
    ASSERT_FALSE(w.lost);
    EXPECT_EQ(live(d), w.nodes.size());

    m.roots().clear();
    d.collect_full();
    EXPECT_TRUE(tnode::registry.empty());
}

INSTANTIATE_TEST_SUITE_P(Seeds, RandomGraph, ::testing::Range<std::uint32_t>(1, 41));
