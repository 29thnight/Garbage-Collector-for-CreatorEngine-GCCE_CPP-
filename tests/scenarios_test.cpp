// Ordinary C++ data structures written on top of the GC references.

#include "support.hpp"

#include <algorithm>
#include <functional>
#include <list>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using gctest::live;

// ------------------------------------------------------------ linked list

namespace
{
struct list_node
{
    int value;
    gc::trace_ref<list_node> next;
    gc::trace_ref<list_node> prev; // strong both ways: every link is a cycle
    explicit list_node(int v) : value(v) {}
    void gc_trace(gc::tracer& t) const
    {
        t.visit(next);
        t.visit(prev);
    }
};

struct linked_list
{
    gc::trace_ref<list_node> head;
    gc::trace_ref<list_node> tail;
    std::size_t size = 0;

    void push_back(gc::domain& d, int v)
    {
        auto n = gc::make<list_node>(d, v);
        n->prev = tail;
        if (tail)
            tail->next = n;
        else
            head = n;
        tail = n;
        ++size;
    }

    void erase_if(const std::function<bool(int)>& pred)
    {
        for (list_node* cur = head.get(); cur;)
        {
            list_node* next = cur->next.get();
            if (pred(cur->value))
            {
                (cur->prev ? cur->prev->next : head) = cur->next;
                (cur->next ? cur->next->prev : tail) = cur->prev;
                --size;
            }
            cur = next;
        }
    }

    std::vector<int> values() const
    {
        std::vector<int> out;
        for (const list_node* cur = head.get(); cur; cur = cur->next.get())
            out.push_back(cur->value);
        return out;
    }

    void gc_trace(gc::tracer& t) const
    {
        t.visit(head);
        t.visit(tail);
    }
};
} // namespace

TEST(Scenarios, DoublyLinkedList)
{
    gc::domain d;
    auto list = gc::make<linked_list>(d);
    for (int i = 0; i < 100; ++i)
        list->push_back(d, i);

    list->erase_if([](int v) { return v % 2 == 1; });
    EXPECT_EQ(d.collect_full().reclaimed, 50u); // removed nodes still point into the list
    EXPECT_EQ(list->size, 50u);

    auto values = list->values();
    ASSERT_EQ(values.size(), 50u);
    for (std::size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(values[i], static_cast<int>(i * 2));

    list = nullptr;
    EXPECT_EQ(d.collect_full().reclaimed, 51u);
}

// ------------------------------------------------------- binary search tree

namespace
{
struct tree_node
{
    int key;
    gc::trace_ref<tree_node> left;
    gc::trace_ref<tree_node> right;
    gc::weak_ref<tree_node> parent; // back reference does not own
    explicit tree_node(int k) : key(k) {}
    void gc_trace(gc::tracer& t) const
    {
        t.visit(left);
        t.visit(right);
    }
};

struct bst
{
    gc::trace_ref<tree_node> root;
    void gc_trace(gc::tracer& t) const { t.visit(root); }

    void insert(gc::domain& d, int key)
    {
        auto n = gc::make<tree_node>(d, key);
        gc::trace_ref<tree_node>* slot = &root;
        const gc::trace_ref<tree_node>* parent = nullptr;
        while (*slot)
        {
            parent = slot;
            slot = key < (*slot)->key ? &(*slot)->left : &(*slot)->right;
        }
        if (parent)
            n->parent = *parent;
        *slot = n;
    }

    gc::trace_ref<tree_node> find_ref(int key) const
    {
        const gc::trace_ref<tree_node>* cur = &root;
        while (*cur && (*cur)->key != key)
            cur = key < (*cur)->key ? &(*cur)->left : &(*cur)->right;
        return *cur;
    }

    // Removes the subtree rooted at key.
    void cut(int key)
    {
        gc::trace_ref<tree_node>* cur = &root;
        while (*cur && (*cur)->key != key)
            cur = key < (*cur)->key ? &(*cur)->left : &(*cur)->right;
        *cur = nullptr;
    }

    static std::size_t count(const tree_node* n) { return n ? 1 + count(n->left.get()) + count(n->right.get()) : 0; }
};
} // namespace

TEST(Scenarios, BinarySearchTreeWithWeakParents)
{
    gc::domain d;
    auto tree = gc::make<bst>(d);
    std::mt19937 rng(7);
    std::set<int> keys;
    while (keys.size() < 500)
        keys.insert(static_cast<int>(rng() % 100000));
    std::vector<int> order(keys.begin(), keys.end());
    std::shuffle(order.begin(), order.end(), rng);
    for (int k : order)
        tree->insert(d, k);

    d.collect_full();
    EXPECT_EQ(live(d), 501u);

    // Parent links resolve while the parent is alive.
    tree_node* deep = tree->root.get();
    while (deep->left)
        deep = deep->left.get();
    auto parent = deep->parent.lock();
    ASSERT_TRUE(parent);
    EXPECT_EQ(parent->left.get(), deep);
    parent = nullptr; // a root would keep part of the subtree cut below

    // Cutting a subtree reclaims exactly that subtree; weak parents of the
    // removed nodes do not keep anything alive.
    const int victim = tree->root->left->key;
    const std::size_t removed = bst::count(tree->root->left.get());
    gc::weak_ref<tree_node> victim_ref = tree->find_ref(victim);
    tree->cut(victim);
    EXPECT_EQ(d.collect_full().reclaimed, removed);
    EXPECT_TRUE(victim_ref.expired());
    EXPECT_EQ(live(d), 501u - removed);
    EXPECT_EQ(bst::count(tree->root.get()), 500u - removed);
}

// ------------------------------------------------------------------ graph

namespace
{
struct vertex
{
    std::string name;
    std::vector<gc::trace_ref<vertex>> out;
    explicit vertex(std::string n) : name(std::move(n)) {}
    void gc_trace(gc::tracer& t) const { t.visit(out); }
};
} // namespace

TEST(Scenarios, GraphComponentsAreReclaimedWhenDisconnected)
{
    gc::domain d;
    std::unordered_map<std::string, gc::weak_ref<vertex>> by_name;
    auto add = [&](const std::string& n) {
        auto v = gc::make<vertex>(d, n);
        by_name[n] = v;
        return v;
    };

    auto entry = add("entry");
    auto a = add("a");
    auto b = add("b");
    auto c = add("c");
    auto x = add("x");
    auto y = add("y");
    entry->out = {gc::trace_ref<vertex>(a)};
    a->out = {gc::trace_ref<vertex>(b), gc::trace_ref<vertex>(c)};
    b->out = {gc::trace_ref<vertex>(c)};
    c->out = {gc::trace_ref<vertex>(a), gc::trace_ref<vertex>(x)}; // cycle a-b-c
    x->out = {gc::trace_ref<vertex>(y)};
    y->out = {gc::trace_ref<vertex>(x)};
    a = b = c = x = y = nullptr;

    d.collect_full();
    EXPECT_EQ(live(d), 6u);

    // Drop c -> x: the x/y cycle becomes unreachable.
    c = by_name["c"].lock();
    c->out.pop_back();
    c = nullptr;
    EXPECT_EQ(d.collect_full().reclaimed, 2u);
    EXPECT_TRUE(by_name["x"].expired());
    EXPECT_TRUE(by_name["y"].expired());

    // Drop the entry edge: the a/b/c cycle goes too.
    entry->out.clear();
    EXPECT_EQ(d.collect_full().reclaimed, 3u);
    EXPECT_EQ(live(d), 1u);
}

// --------------------------------------------------------------- LRU cache

namespace
{
struct texture_like
{
    int key;
    std::vector<unsigned char> pixels = std::vector<unsigned char>(256);
    explicit texture_like(int k) : key(k) {}
};

class lru_cache
{
public:
    explicit lru_cache(std::size_t capacity) : capacity_(capacity) {}

    gc::root_ref<texture_like> get(gc::domain& d, int key)
    {
        if (auto it = index_.find(key); it != index_.end())
        {
            order_.splice(order_.begin(), order_, it->second);
            return gc::root_ref<texture_like>(it->second->second);
        }
        order_.emplace_front(key, gc::make<texture_like>(d, key));
        index_[key] = order_.begin();
        if (order_.size() > capacity_)
        {
            index_.erase(order_.back().first);
            order_.pop_back();
        }
        return gc::root_ref<texture_like>(order_.front().second);
    }

    void gc_trace(gc::tracer& t) const { t.visit(order_); }

private:
    using entry = std::pair<int, gc::trace_ref<texture_like>>;
    std::size_t capacity_;
    std::list<entry> order_;
    std::unordered_map<int, std::list<entry>::iterator> index_;
};
} // namespace

TEST(Scenarios, LruCacheEvictionDoesNotFreeEntriesStillInUse)
{
    gc::domain d;
    auto cache = gc::make<lru_cache>(d, 4);

    auto held = cache->get(d, 1); // an outside user keeps entry 1
    for (int k = 2; k <= 10; ++k)
        (void)cache->get(d, k);

    d.collect_full();
    EXPECT_EQ(live(d), 1u + 4u + 1u); // cache, 4 cached entries, evicted but held entry 1
    EXPECT_EQ(held->key, 1);

    auto again = cache->get(d, 1); // a miss creates a new entry
    EXPECT_FALSE(again == held);

    held = nullptr;
    again = nullptr;
    d.collect_full();
    EXPECT_EQ(live(d), 1u + 4u);
}

// ---------------------------------------------------------------- observer

namespace
{
struct listener
{
    int received = 0;
};

struct subject
{
    std::vector<gc::weak_ref<listener>> listeners; // observers are not owned

    int notify()
    {
        int delivered = 0;
        std::erase_if(listeners, [](const auto& w) { return w.expired(); });
        for (const auto& w : listeners)
            if (auto l = w.lock())
            {
                ++l->received;
                ++delivered;
            }
        return delivered;
    }
};
} // namespace

TEST(Scenarios, ObserverWithWeakSubscriptions)
{
    gc::domain d;
    auto s = gc::make<subject>(d);
    std::vector<gc::root_ref<listener>> owners;
    for (int i = 0; i < 10; ++i)
    {
        owners.push_back(gc::make<listener>(d));
        s->listeners.push_back(owners.back());
    }
    EXPECT_EQ(s->notify(), 10);

    owners.erase(owners.begin(), owners.begin() + 6);
    d.collect_full();
    EXPECT_EQ(s->notify(), 4);
    EXPECT_EQ(s->listeners.size(), 4u);
    for (const auto& o : owners)
        EXPECT_EQ(o->received, 2);
}

// ---------------------------------------------------------------- closures

TEST(Scenarios, ClosuresHoldingRootsKeepObjectsAlive)
{
    gc::domain d;
    std::vector<std::function<int()>> callbacks;
    gc::weak_ref<gctest::node> watch;
    {
        auto n = gc::make<gctest::node>(d, 42);
        watch = n;
        callbacks.push_back([n] { return n->id; });
    }
    d.collect_full();
    ASSERT_FALSE(watch.expired());
    EXPECT_EQ(callbacks.front()(), 42);

    callbacks.clear();
    d.collect_full();
    EXPECT_TRUE(watch.expired());
}
