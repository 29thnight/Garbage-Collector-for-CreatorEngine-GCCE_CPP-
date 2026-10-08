#pragma once

#include "managed.hpp"
#include "ref_from_this.hpp"
#include "tracer.hpp"

#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace gc
{
namespace detail
{
template <class T>
void trace_thunk(const void* object, tracer& t)
{
    if constexpr (has_gc_trace<T>)
        static_cast<const T*>(object)->gc_trace(t);
}

template <class T>
void destroy_thunk(void* object) noexcept
{
    static_cast<T*>(object)->~T();
}

template <class T>
const type_descriptor& descriptor_for()
{
    static const type_descriptor descriptor{typeid(T).name(), sizeof(T), alignof(T), &trace_thunk<T>,
                                            &destroy_thunk<T>};
    return descriptor;
}
} // namespace detail

// Allocates and constructs a GC-managed T in domain d.
//
// The partially constructed object is never visible to the tracer: it is
// published, with its type and slot, only after the constructor returns.
// A collection cannot start while a constructor runs. If the constructor
// throws, the storage is released without running a destructor.
//
// T must derive from gc::managed; the object is owned by the GC alone.
template <class T, class... Args>
[[nodiscard]] root_ref<T> make(domain& d, Args&&... args)
{
    static_assert(std::is_object_v<T> && !std::is_array_v<T>, "gc::make: T must be a non-array object type");
    static_assert(std::is_base_of_v<managed, T>,
                  "gc::make: T must derive from gc::managed, which keeps new and delete away from GC objects");
    static_assert(std::is_nothrow_destructible_v<T>, "gc::make: destructors of GC objects must not throw");

    auto pending = detail::domain_access::begin_allocation(d, detail::descriptor_for<T>());
    T* object = nullptr;
    try
    {
        object = ::new (pending.header->object) T(std::forward<Args>(args)...);
    }
    catch (...)
    {
        detail::domain_access::abort_allocation(d, pending);
        throw;
    }
    if constexpr (std::is_base_of_v<detail::ref_from_this_base, T>)
        static_cast<detail::ref_from_this_base*>(object)->gc_self_header_ = pending.header;
    detail::object_header* header = detail::domain_access::publish(d, pending);
    return detail::ref_access::make_root<T>(header, object);
}
} // namespace gc
