#pragma once

#include "config.hpp"

#include <cstddef>
#include <new>

namespace gc
{
namespace detail
{
[[noreturn]] GC_API void managed_delete_called() noexcept;
}

// Base of every GC-managed type. GC objects are created only by gc::make
// and destroyed only by the collector, so the class-scope allocation
// functions shut out every other path at compile time:
//
//     new T, new T[n], new (std::nothrow) T, new (p) T   -> error
//     delete p, std::unique_ptr<T>, std::make_unique<T>  -> error
//
// operator delete cannot be deleted or private: a virtual destructor looks
// it up, so polymorphic GC types would stop compiling. It is protected and
// aborts if reached, which only `delete this` inside the class can do.
// A deliberately global ::new bypasses class scope and is not prevented.
class managed
{
public:
    static void* operator new(std::size_t) = delete;
    static void* operator new[](std::size_t) = delete;
    static void* operator new(std::size_t, std::align_val_t) = delete;
    static void* operator new[](std::size_t, std::align_val_t) = delete;

protected:
    managed() noexcept = default;
    managed(const managed&) noexcept = default;
    managed& operator=(const managed&) noexcept = default;
    ~managed() = default;

    static void operator delete(void*) noexcept { detail::managed_delete_called(); }
    static void operator delete[](void*) noexcept { detail::managed_delete_called(); }
    static void operator delete(void*, std::align_val_t) noexcept { detail::managed_delete_called(); }
    static void operator delete[](void*, std::align_val_t) noexcept { detail::managed_delete_called(); }
};
} // namespace gc
