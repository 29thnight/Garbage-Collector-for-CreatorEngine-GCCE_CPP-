#include "test_framework.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using gctest::node;

namespace
{
// Reachability computed from the current graph without the collector.
std::unordered_set<int> oracle_reachable(const std::vector<gc::root_ref<node>>& roots)
{
    std::unordered_set<int> seen;
    std::vector<const node*> stack;
    for (const auto& r : roots)
        if (r)
            stack.push_back(r.get());
    while (!stack.empty())
    {
        const node* n = stack.back();
        stack.pop_back();
        if (!seen.insert(n->id).second)
            continue;
        if (n->next)
            stack.push_back(n->next.get());
        for (const auto& c : n->children)
            if (c)
                stack.push_back(c.get());
    }
    return seen;
}

void run_random_graph(std::uint32_t seed)
{
    const std::string where = "seed=" + std::to_string(seed);
    std::mt19937 rng(seed);
    auto pick = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng); };

    gc::domain d;
    std::vector<gc::root_ref<node>> roots;
    std::vector<gc::weak_ref<node>> handles; // index == node id
    int next_id = 0;

    for (int round = 0; round < 40; ++round)
    {
        // Allocate
        for (int i = 0; i < 20; ++i)
        {
            auto n = gc::make<node>(d, next_id++);
            handles.push_back(n);
            if (pick(4) == 0)
                roots.push_back(n);
            else if (!roots.empty())
                roots[pick(roots.size())]->children.push_back(n);
        }

        // Mutate edges among live nodes
        std::vector<gc::root_ref<node>> live;
        for (const auto& h : handles)
            if (auto r = h.lock())
                live.push_back(std::move(r));
        for (int i = 0; i < 30 && !live.empty(); ++i)
        {
            auto& from = live[pick(live.size())];
            switch (pick(4))
            {
            case 0: from->next = live[pick(live.size())]; break;
            case 1: from->next = nullptr; break;
            case 2: from->children.push_back(live[pick(live.size())]); break;
            case 3:
                if (!from->children.empty())
                    from->children.erase(from->children.begin() +
                                         static_cast<std::ptrdiff_t>(pick(from->children.size())));
                break;
            }
        }
        live.clear();

        // Drop or add roots
        if (!roots.empty() && pick(2) == 0)
            roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(pick(roots.size())));
        if (pick(3) == 0 && !handles.empty())
            if (auto r = handles[pick(handles.size())].lock())
                roots.push_back(std::move(r));

        const auto expected = oracle_reachable(roots);
        const auto result = d.collect_full();
        if (!result.completed)
            gctest::fail(__FILE__, __LINE__, where + ": cycle did not complete");

        for (int id = 0; id < next_id; ++id)
        {
            const bool alive = !handles[static_cast<std::size_t>(id)].expired();
            const bool should = expected.count(id) != 0;
            if (should && !alive)
                gctest::fail(__FILE__, __LINE__,
                             where + " round=" + std::to_string(round) + ": reachable node " +
                                 std::to_string(id) + " was reclaimed");
            if (!should && alive)
                gctest::fail(__FILE__, __LINE__,
                             where + " round=" + std::to_string(round) + ": garbage node " +
                                 std::to_string(id) + " survived a full cycle");
        }
        if (d.stats().live_objects != expected.size())
            gctest::fail(__FILE__, __LINE__, where + ": live object count mismatch");
    }

    roots.clear();
    d.collect_full();
    if (d.stats().live_objects != 0)
        gctest::fail(__FILE__, __LINE__, where + ": objects left after dropping all roots");
}
} // namespace

// C12 (synchronous baseline): random graphs compared with an independent
// reachability oracle. The seed is part of every failure message.
GC_TEST(c12_random_graphs_match_oracle)
{
    for (std::uint32_t seed = 1; seed <= 25; ++seed)
        run_random_graph(seed);
}
