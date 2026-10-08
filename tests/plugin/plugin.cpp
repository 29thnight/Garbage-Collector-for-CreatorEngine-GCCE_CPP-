// A separately loaded module that defines its own GC type, for the module
// unload test. Built only with the shared runtime.
#include <gc/gc.hpp>

#if defined(_WIN32)
#    define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#    define PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
struct plugin_object : gc::managed
{
    int value = 42;
    gc::trace_ref<plugin_object> next;
    void gc_trace(gc::tracer& t) const { t.visit(next); }
};
} // namespace

PLUGIN_EXPORT void plugin_make_objects(gc::domain& d, int count, gc::root_ref<gc::managed>* out)
{
    auto head = gc::make<plugin_object>(d);
    for (int i = 1; i < count; ++i)
    {
        auto n = gc::make<plugin_object>(d);
        n->next = head;
        head = n;
    }
    *out = head;
}
