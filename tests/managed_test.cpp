#include "support.hpp"

#include <new>
#include <type_traits>

using gctest::node;

namespace
{
template <class T>
concept newable = requires { new T; };
template <class T>
concept array_newable = requires { new T[2]; };
template <class T>
concept nothrow_newable = requires { new (std::nothrow) T; };
template <class T>
concept placement_newable = requires(void* p) { new (p) T; };
template <class T>
concept deletable = requires(T* p) { delete p; };

struct polymorphic : gc::managed
{
    virtual ~polymorphic() = default;
};
struct derived_polymorphic final : polymorphic
{
};
struct alignas(64) over_aligned : gc::managed
{
    int v = 0;
};
struct plain
{
};

struct self_deleting : gc::managed
{
    void destroy() { delete this; }
};
} // namespace

// The allocation and deallocation forms outside gc::make are rejected at
// compile time for every kind of GC type.
static_assert(!newable<node> && !newable<polymorphic> && !newable<derived_polymorphic> && !newable<over_aligned>);
static_assert(!array_newable<node> && !array_newable<derived_polymorphic>);
static_assert(!nothrow_newable<node> && !nothrow_newable<derived_polymorphic>);
static_assert(!placement_newable<node> && !placement_newable<derived_polymorphic>);
// delete and unique_ptr are checked by the CompileFail.delete_expression and
// CompileFail.unique_ptr build tests: GCC does not treat the inaccessible
// operator delete as a substitution failure inside a requires-expression,
// although the real delete expression is rejected.

// Ordinary types are unaffected.
static_assert(newable<plain> && deletable<plain>);

// gc::managed adds no storage.
static_assert(std::is_empty_v<gc::managed>);

TEST(Managed, PolymorphicTypesStillWork)
{
    gc::domain d;
    gc::root_ref<polymorphic> p = gc::make<derived_polymorphic>(d);
    EXPECT_TRUE(gc::dynamic_ref_cast<derived_polymorphic>(p));
    p = nullptr;
    EXPECT_EQ(d.collect_full().reclaimed, 1u);
}

TEST(ManagedDeathTest, DeleteThisAborts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            gc::domain d;
            auto obj = gc::make<self_deleting>(d);
            obj->destroy();
        },
        "destroyed only by the collector");
}
