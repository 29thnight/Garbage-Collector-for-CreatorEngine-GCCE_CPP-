#include "support.hpp"

#include <string>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

using gctest::node;

namespace
{
void anchor_in_test_module() {}
} // namespace

TEST(Modules, CountsObjectsByTheModuleOfTheirType)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> keep;
    for (int i = 0; i < 10; ++i)
        keep.push_back(gc::make<node>(d, i));
    // node is instantiated in the test executable.
    EXPECT_EQ(d.objects_in_module(reinterpret_cast<const void*>(&anchor_in_test_module)), 10u);
    keep.clear();
    d.collect_full();
    EXPECT_EQ(d.objects_in_module(reinterpret_cast<const void*>(&anchor_in_test_module)), 0u);
}

#if defined(GCCE_SHARED) && defined(GCCE_TEST_PLUGIN)
namespace
{
using make_fn = void (*)(gc::domain&, int, gc::root_ref<gc::managed>*);

struct loaded_module
{
#    if defined(_WIN32)
    HMODULE handle = ::LoadLibraryA(GCCE_TEST_PLUGIN);
    ~loaded_module()
    {
        if (handle)
            ::FreeLibrary(handle);
    }
    void* symbol(const char* name) const { return reinterpret_cast<void*>(::GetProcAddress(handle, name)); }
#    else
    void* handle = ::dlopen(GCCE_TEST_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    ~loaded_module()
    {
        if (handle)
            ::dlclose(handle);
    }
    void* symbol(const char* name) const { return ::dlsym(handle, name); }
#    endif
};
} // namespace

// A module's objects are counted against it until they are reclaimed; then
// it can be unloaded.
TEST(Modules, PluginObjectsMustBeGoneBeforeUnload)
{
    gc::domain d;
    auto own = gc::make<node>(d, 1);
    {
        loaded_module plugin;
        ASSERT_NE(plugin.handle, nullptr);
        auto make = reinterpret_cast<make_fn>(plugin.symbol("plugin_make_objects"));
        ASSERT_NE(make, nullptr);
        const void* in_plugin = reinterpret_cast<const void*>(make);

        gc::root_ref<gc::managed> objects;
        make(d, 5, &objects);
        EXPECT_EQ(d.objects_in_module(in_plugin), 5u);
        EXPECT_EQ(d.objects_in_module(reinterpret_cast<const void*>(&anchor_in_test_module)), 1u);

        d.collect_full();
        EXPECT_EQ(d.objects_in_module(in_plugin), 5u); // still referenced

        objects = nullptr;
        d.collect_full(); // runs the plugin's destructors while it is loaded
        EXPECT_EQ(d.objects_in_module(in_plugin), 0u);
    } // unloaded here
    d.collect_full();
    EXPECT_EQ(own->id, 1);
}
#endif
