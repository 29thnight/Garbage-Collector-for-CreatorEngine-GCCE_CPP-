// Must not compile: gc::make of a type that derives from gc::managed twice.
// GCCE_COMPILE_FAIL_CONTROL removes the offending line; the control build
// must succeed, so a failure here is caused by that line alone.
#include <gc/gc.hpp>

struct left : gc::managed
{
};

struct right : gc::managed
{
};

struct twice : left, right
{
};

struct once : left
{
};

void use(gc::domain& d)
{
    auto ok = gc::make<once>(d);
    (void)ok;
#if !defined(GCCE_COMPILE_FAIL_CONTROL)
    auto r = gc::make<twice>(d); (void)r;
#endif
}
