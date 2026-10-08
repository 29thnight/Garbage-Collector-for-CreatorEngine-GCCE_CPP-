// Must not compile: unique_ptr owning a GC object
// GCCE_COMPILE_FAIL_CONTROL removes the offending line; the control build
// must succeed, so a failure here is caused by that line alone.
#include <gc/gc.hpp>

#include <memory>
#include <new>

struct object : gc::managed
{
    virtual ~object() = default;
    int value = 0;
};

struct plain
{
    int value = 0;
};

void use(gc::domain& d, object* existing, void* buffer)
{
    (void)d;
    (void)existing;
    (void)buffer;
#if !defined(GCCE_COMPILE_FAIL_CONTROL)
    std::unique_ptr<object> p(existing);
#endif
}
