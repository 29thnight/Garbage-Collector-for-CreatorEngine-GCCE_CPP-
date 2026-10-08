#pragma once

#include "refs.hpp"

namespace gc
{
namespace detail
{
struct ref_from_this_base
{
    object_header* gc_self_header_ = nullptr;
};
} // namespace detail

// Lets a GC object obtain references to itself, like
// std::enable_shared_from_this. Inherit publicly, next to gc::managed:
//
//     struct widget : gc::managed, gc::enable_ref_from_this<widget> { ... };
//
// gc::make sets the identity after the constructor returns, so inside the
// constructor (and for objects not created by gc::make) root_from_this()
// returns an empty reference.
template <class T>
class enable_ref_from_this : public detail::ref_from_this_base
{
public:
    [[nodiscard]] root_ref<T> root_from_this()
    {
        return detail::ref_access::make_root<T>(gc_self_header_, gc_self_header_ ? static_cast<T*>(this) : nullptr);
    }

    [[nodiscard]] weak_ref<T> weak_from_this() noexcept
    {
        return detail::ref_access::make_weak<T>(gc_self_header_, static_cast<T*>(this));
    }

protected:
    enable_ref_from_this() noexcept = default;
    // Copying an object never copies its GC identity.
    enable_ref_from_this(const enable_ref_from_this&) noexcept : detail::ref_from_this_base() {}
    enable_ref_from_this& operator=(const enable_ref_from_this&) noexcept { return *this; }
    ~enable_ref_from_this() = default;
};
} // namespace gc
