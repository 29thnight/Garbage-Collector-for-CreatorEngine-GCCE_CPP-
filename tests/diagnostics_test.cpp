#include "support.hpp"

#include <algorithm>
#include <string>
#include <vector>

using gctest::make_chain;
using gctest::node;

namespace
{
struct leaf : gc::managed
{
};
} // namespace

TEST(Diagnostics, ForEachRootReportsTypesAndLabels)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    a.set_label("scene-list");
    auto b = gc::make<leaf>(d);

    std::vector<std::string> labels;
    std::vector<std::string> types;
    d.for_each_root([&](const gc::object_info& info) {
        labels.push_back(info.root_label ? info.root_label : "");
        types.push_back(info.type_name);
    });
    ASSERT_EQ(labels.size(), 2u);
    EXPECT_NE(std::find(labels.begin(), labels.end(), "scene-list"), labels.end());
    bool saw_node = false;
    bool saw_leaf = false;
    for (const auto& t : types)
    {
        saw_node |= t.find("node") != std::string::npos;
        saw_leaf |= t.find("leaf") != std::string::npos;
    }
    EXPECT_TRUE(saw_node);
    EXPECT_TRUE(saw_leaf);
}

TEST(Diagnostics, RetentionPathExplainsWhyAnObjectIsAlive)
{
    gc::domain d;
    gc::root_ref<node> root;
    auto chain = make_chain(d, root, 4);
    root.set_label("owner");
    const gc::trace_ref<node>& last_edge = chain[3]->next; // c4

    auto path = gc::retention_path(last_edge);
    ASSERT_EQ(path.size(), 5u); // root, c1, c2, c3, c4
    EXPECT_STREQ(path.front().root_label, "owner");
    for (std::size_t i = 1; i < path.size(); ++i)
        EXPECT_EQ(path[i].root_label, nullptr);
    EXPECT_EQ(path.back().size, path.front().size);
}

TEST(Diagnostics, RetentionPathOfUnreachableObjectIsEmpty)
{
    gc::domain d;
    gc::root_ref<node> keep = gc::make<node>(d, 1);
    gc::trace_ref<node> orphan_edge;
    {
        auto orphan = gc::make<node>(d, 2);
        keep->next = orphan;
        orphan_edge = keep->next;
        keep->next = nullptr;
    }
    // orphan_edge is a stray trace_ref outside any GC object: not a root.
    EXPECT_TRUE(gc::retention_path(orphan_edge).empty());
    EXPECT_EQ(gc::retention_path(keep).size(), 1u);
}

TEST(Diagnostics, QuarantinedObjectsAreListedAndExplained)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    gc::weak_ref<node> handle;
    {
        auto v = gc::make<node>(d, 1);
        v->next = gc::make<node>(d, 2);
        ASSERT_TRUE(gc::begin_cleanup_obligation(v));
        handle = v;
    }
    d.collect_full();

    std::size_t listed = 0;
    d.for_each_quarantined([&](const gc::object_info& info) {
        ++listed;
        EXPECT_TRUE(info.quarantined);
        EXPECT_EQ(info.lifecycle, gc::lifecycle_state::alive);
    });
    EXPECT_EQ(listed, 1u);

    // A trace_ref copy outside any object is not a root, so the only path is
    // the quarantine (a root_ref here would itself be the shortest path).
    gc::trace_ref<node> child = handle.lock()->next;
    auto path = gc::retention_path(child);
    ASSERT_EQ(path.size(), 2u);
    EXPECT_STREQ(path.front().root_label, "gc.quarantine");
    EXPECT_TRUE(path.front().quarantined);
}

TEST(Diagnostics, CycleTimingIsRecorded)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    for (int i = 0; i < 1000; ++i)
        keep.push_back(gc::make<node>(d, i));
    for (int i = 0; i < 1000; ++i)
        (void)gc::make<node>(d, i);
    auto r = d.collect_full();
    EXPECT_EQ(r.reclaimed, 1000u);
    EXPECT_GT(r.mark_time.count() + r.sweep_time.count(), 0);
    EXPECT_GE(d.stats().worst_finalize.count(), 0);
}
