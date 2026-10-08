#include "support.hpp"

#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using gctest::live;
using gctest::node;

namespace
{
struct animal : gc::managed
{
    virtual ~animal() = default;
    virtual std::string sound() const = 0;
    void gc_trace(gc::tracer&) const {}
};
struct dog final : animal
{
    std::string sound() const override { return "woof"; }
};
struct cat final : animal
{
    std::string sound() const override { return "meow"; }
};

struct left_base
{
    int l = 1;
    virtual ~left_base() = default;
};
struct right_base
{
    int r = 2;
    virtual ~right_base() = default;
};
struct both : gc::managed, left_base, right_base
{
};

struct widget : gc::managed
{
    gc::root_ref<widget> seen_in_constructor;
    widget() { seen_in_constructor = root_from_this(); }

    // A typical use: hand a reference to yourself to another object.
    void attach_to(node& parent_like, std::vector<gc::trace_ref<widget>>& registry)
    {
        (void)parent_like;
        registry.push_back(root_from_this());
    }
};

struct panel_base : gc::managed
{
    virtual ~panel_base() = default;
    gc::root_ref<panel_base> base_self() { return root_from_this(); }
    gc::weak_ref<panel_base> base_weak_self() { return weak_from_this(); }
};

struct titled
{
    std::string title = "t";
    virtual ~titled() = default;
};

// The managed base is not the first base: its address differs from the
// most derived object's.
struct titled_panel final : titled, panel_base
{
    gc::root_ref<titled_panel> self() { return root_from_this(); }
};
} // namespace

TEST(RootRef, DefaultAndNullAreEmpty)
{
    gc::root_ref<node> a;
    gc::root_ref<node> b = nullptr;
    EXPECT_FALSE(a);
    EXPECT_TRUE(a == nullptr);
    EXPECT_EQ(b.get(), nullptr);
    EXPECT_TRUE(a == b);
}

TEST(RootRef, CopyMoveAndResetKeepRootCountExact)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    EXPECT_EQ(d.stats().roots, 1u);

    gc::root_ref<node> copy = a;
    EXPECT_EQ(d.stats().roots, 2u);
    EXPECT_TRUE(copy == a);

    gc::root_ref<node> moved = std::move(a);
    EXPECT_FALSE(a);
    EXPECT_EQ(d.stats().roots, 2u);

    copy = moved; // same target: no new registration
    EXPECT_EQ(d.stats().roots, 2u);
    const auto& alias = copy;
    copy = alias;
    EXPECT_EQ(d.stats().roots, 2u);

    copy.reset();
    EXPECT_EQ(d.stats().roots, 1u);
    moved = nullptr;
    EXPECT_EQ(d.stats().roots, 0u);
}

// C02: vector relocation and erase move every element; no root is lost and
// no stale registration remains.
TEST(RootRef, ContainerRelocationKeepsTargetsProtected)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> roots;
    for (int i = 0; i < 1000; ++i)
        roots.push_back(gc::make<node>(d, i));
    EXPECT_EQ(d.stats().roots, 1000u);

    d.collect_full();
    EXPECT_EQ(live(d), 1000u);

    roots.erase(roots.begin() + 100, roots.begin() + 600);
    EXPECT_EQ(d.stats().roots, 500u);
    d.collect_full();
    EXPECT_EQ(live(d), 500u);
    EXPECT_EQ(roots[100]->id, 600);

    std::vector<gc::root_ref<node>> other;
    other.swap(roots);
    roots = std::move(other);
    d.collect_full();
    EXPECT_EQ(live(d), 500u);
}

TEST(RootRef, LabelBelongsToTheHolderNotTheValue)
{
    gc::domain d;
    gc::root_ref<node> a = gc::make<node>(d, 1);
    a.set_label("holder-a");
    gc::root_ref<node> b = a;
    EXPECT_STREQ(a.label(), "holder-a");
    EXPECT_EQ(b.label(), nullptr);
    b.set_label("holder-b");
    a = b;
    EXPECT_STREQ(a.label(), "holder-a");
}

TEST(TraceRef, CopyMoveAndReset)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    gc::trace_ref<node> t = a;
    EXPECT_TRUE(t == a);
    EXPECT_EQ(t->id, 1);

    gc::trace_ref<node> moved = std::move(t);
    EXPECT_FALSE(t);
    EXPECT_TRUE(moved == a);

    gc::trace_ref<node> copy = moved;
    copy.reset();
    EXPECT_FALSE(copy);
    EXPECT_TRUE(moved);
    EXPECT_EQ(d.stats().roots, 1u); // trace_refs never register roots
}

TEST(Refs, IdentityComparisonAcrossBaseTypes)
{
    gc::domain d;
    auto obj = gc::make<both>(d);
    gc::root_ref<right_base> as_right = obj;
    gc::trace_ref<left_base> as_left = obj;

    EXPECT_NE(static_cast<void*>(as_right.get()), static_cast<void*>(obj.get()));
    EXPECT_EQ(as_right.get(), static_cast<right_base*>(obj.get()));
    EXPECT_TRUE(as_right == obj);
    EXPECT_TRUE(as_left == as_right);
    EXPECT_TRUE(gc::same_object(as_left, obj));

    auto other = gc::make<both>(d);
    EXPECT_FALSE(other == obj);
}

TEST(Refs, HashByIdentity)
{
    gc::domain d;
    auto a = gc::make<node>(d, 1);
    auto b = gc::make<node>(d, 2);

    std::unordered_set<gc::root_ref<node>> set;
    set.insert(a);
    set.insert(gc::root_ref<node>(a));
    set.insert(b);
    EXPECT_EQ(set.size(), 2u);

    std::unordered_map<gc::trace_ref<node>, int> index;
    index[gc::trace_ref<node>(a)] = 10;
    EXPECT_EQ(index.at(gc::trace_ref<node>(a)), 10);
}

TEST(Refs, StaticAndDynamicCasts)
{
    gc::domain d;
    gc::root_ref<animal> pet = gc::make<dog>(d);
    EXPECT_EQ(pet->sound(), "woof");

    gc::root_ref<dog> as_dog = gc::dynamic_ref_cast<dog>(pet);
    ASSERT_TRUE(as_dog);
    EXPECT_TRUE(as_dog == pet);
    EXPECT_FALSE(gc::dynamic_ref_cast<cat>(pet));

    gc::trace_ref<animal> edge = pet;
    gc::trace_ref<dog> edge_dog = gc::static_ref_cast<dog>(edge);
    EXPECT_EQ(edge_dog.get(), as_dog.get());
    EXPECT_FALSE(gc::dynamic_ref_cast<cat>(edge));

    gc::root_ref<both> mi = gc::make<both>(d);
    gc::root_ref<right_base> right = mi;
    gc::root_ref<both> back = gc::static_ref_cast<both>(right);
    EXPECT_EQ(back.get(), mi.get());

    EXPECT_FALSE(gc::dynamic_ref_cast<dog>(gc::root_ref<animal>{}));
}

TEST(RefFromThis, ReturnsTheSameObject)
{
    gc::domain d;
    auto w = gc::make<widget>(d);
    gc::root_ref<widget> self = w->root_from_this();
    static_assert(std::is_same_v<decltype(self), gc::root_ref<widget>>);
    EXPECT_TRUE(self == w);
    EXPECT_EQ(d.stats().roots, 2u);

    gc::weak_ref<widget> weak = w->weak_from_this();
    EXPECT_TRUE(weak.lock() == w);
    w = nullptr;
    self = nullptr;
    d.collect_full();
    EXPECT_TRUE(weak.expired());
}

TEST(RefFromThis, EmptyInsideTheConstructor)
{
    gc::domain d;
    auto w = gc::make<widget>(d);
    EXPECT_FALSE(w->seen_in_constructor); // identity is set after construction
}

TEST(RefFromThis, MemberFunctionCanRegisterItself)
{
    gc::domain d;
    auto holder = gc::make<node>(d, 0);
    std::vector<gc::trace_ref<widget>> registry;
    {
        auto w = gc::make<widget>(d);
        w->attach_to(*holder, registry);
    }
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_TRUE(registry.front()); // the stray trace_ref itself is not a root
    EXPECT_EQ(d.stats().roots, 1u);
}

// Deducing this: a base-class member function gets a reference typed and
// adjusted for that base, the derived one gets the derived type.
TEST(RefFromThis, FollowsTheTypeOfTheCall)
{
    gc::domain d;
    auto obj = gc::make<titled_panel>(d);
    panel_base* as_base = obj.get();
    ASSERT_NE(static_cast<void*>(as_base), static_cast<void*>(obj.get()));

    gc::root_ref<panel_base> b = obj->base_self();
    EXPECT_EQ(b.get(), as_base);
    EXPECT_TRUE(b == obj);

    gc::root_ref<titled_panel> t = obj->self();
    EXPECT_EQ(t.get(), obj.get());

    gc::root_ref<panel_base> through_pointer = as_base->root_from_this();
    EXPECT_TRUE(through_pointer == obj);

    gc::weak_ref<panel_base> w = obj->base_weak_self();
    obj = nullptr;
    b = nullptr;
    t = nullptr;
    through_pointer = nullptr;
    d.collect_full();
    EXPECT_TRUE(w.expired());
}

TEST(RefFromThis, ConstObjectsGiveConstReferences)
{
    gc::domain d;
    auto w = gc::make<widget>(d);
    const widget& cw = *w;
    auto r = cw.root_from_this();
    static_assert(std::is_same_v<decltype(r), gc::root_ref<const widget>>);
    EXPECT_TRUE(r == w);
}
