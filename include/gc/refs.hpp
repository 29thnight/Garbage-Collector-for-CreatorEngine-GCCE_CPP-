#pragma once

#include "domain.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace gc
{
template <class T> class root_ref;
template <class T> class trace_ref;
template <class T> class weak_ref;

namespace detail
{
struct ref_access
{
    template <class T> static object_header* header(const root_ref<T>& r) noexcept { return r.node_.header; }
    template <class T> static object_header* header(const trace_ref<T>& r) noexcept { return r.header_; }
    template <class T> static T* raw(const root_ref<T>& r) noexcept { return r.ptr_; }
    template <class T> static T* raw(const trace_ref<T>& r) noexcept { return r.ptr_; }

    template <class T> static root_ref<T> make_root(object_header* h, T* p) { return root_ref<T>(h, p); }
};

template <class From, class To>
concept ref_convertible = std::convertible_to<From*, To*>;
} // namespace detail

// External root. Keeps its target and everything reachable from it alive.
// Owner thread only. Not for members of GC-managed objects: a root inside an
// object would keep the graph alive after the object becomes unreachable.
// Releasing the last root_ref never requests logical destruction.
template <class T>
class root_ref
{
public:
    using element_type = T;

    root_ref() noexcept = default;
    root_ref(std::nullptr_t) noexcept {}

    root_ref(const root_ref& other) { assign(other.node_.header, other.ptr_); }
    // The new root is linked before the source is released, so the target is
    // never unprotected during the move.
    root_ref(root_ref&& other) { assign(other.node_.header, other.ptr_); other.reset(); }

    template <class U>
        requires detail::ref_convertible<U, T>
    root_ref(const root_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    root_ref(root_ref<U>&& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
        other.reset();
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    root_ref(const trace_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
    }

    ~root_ref() { reset(); }

    root_ref& operator=(const root_ref& other)
    {
        if (this != &other)
            assign(other.node_.header, other.ptr_);
        return *this;
    }

    root_ref& operator=(root_ref&& other)
    {
        if (this != &other)
        {
            assign(other.node_.header, other.ptr_);
            other.reset();
        }
        return *this;
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    root_ref& operator=(const root_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
        return *this;
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    root_ref& operator=(const trace_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
        return *this;
    }

    root_ref& operator=(std::nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    void reset() noexcept
    {
        if (node_.header)
        {
            detail::domain_access::unlink_root(&node_);
            node_.header = nullptr;
        }
        ptr_ = nullptr;
    }

    [[nodiscard]] T* get() const noexcept { return node_.header ? ptr_ : nullptr; }
    T& operator*() const noexcept { return *get(); }
    T* operator->() const noexcept { return get(); }
    explicit operator bool() const noexcept { return node_.header != nullptr; }

    friend bool operator==(const root_ref& r, std::nullptr_t) noexcept { return !r; }

private:
    template <class> friend class root_ref;
    friend struct detail::ref_access;

    root_ref(detail::object_header* h, T* p) { assign(h, p); }

    void assign(detail::object_header* h, T* p)
    {
        if (h == nullptr)
        {
            reset();
            return;
        }
        detail::domain_access::on_store(h);
        if (node_.header && node_.header->owner == h->owner)
        {
            // Retarget in place; the node stays in the same root list.
            node_.header = h;
            ptr_ = p;
            return;
        }
        if (node_.header)
            detail::domain_access::unlink_root(&node_);
        node_.header = h;
        ptr_ = p;
        detail::domain_access::link_root(&node_);
    }

    detail::root_node node_;
    T* ptr_ = nullptr;
};

// Strong edge held by a GC-managed object. Followed while its owner is
// reachable. Every non-null store goes through the insertion barrier; do not
// bypass it with memcpy or raw overwrites. Destruction never touches the
// target, so destructors running during sweep may drop already reclaimed
// targets safely.
template <class T>
class trace_ref
{
public:
    using element_type = T;

    trace_ref() noexcept = default;
    trace_ref(std::nullptr_t) noexcept {}

    trace_ref(const trace_ref& other) { assign(other.header_, other.ptr_); }
    trace_ref(trace_ref&& other)
    {
        assign(other.header_, other.ptr_);
        other.reset();
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    trace_ref(const trace_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    trace_ref(const root_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
    }

    ~trace_ref() = default;

    trace_ref& operator=(const trace_ref& other)
    {
        assign(other.header_, other.ptr_);
        return *this;
    }

    trace_ref& operator=(trace_ref&& other)
    {
        if (this != &other)
        {
            assign(other.header_, other.ptr_);
            other.reset();
        }
        return *this;
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    trace_ref& operator=(const trace_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
        return *this;
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    trace_ref& operator=(const root_ref<U>& other)
    {
        assign(detail::ref_access::header(other), detail::ref_access::raw(other));
        return *this;
    }

    trace_ref& operator=(std::nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    void reset() noexcept
    {
        header_ = nullptr;
        ptr_ = nullptr;
    }

    [[nodiscard]] T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return header_ != nullptr; }

    friend bool operator==(const trace_ref& r, std::nullptr_t) noexcept { return !r; }

private:
    template <class> friend class trace_ref;
    friend struct detail::ref_access;

    void assign(detail::object_header* h, T* p)
    {
        if (h)
            detail::domain_access::on_store(h);
        header_ = h;
        ptr_ = p;
    }

    detail::object_header* header_ = nullptr;
    T* ptr_ = nullptr;
};

// Non-owning reference. Promotion with lock() yields a root_ref, which means
// the memory is protected; whether the engine may still use the object is a
// separate lifecycle check. Promotion touches the root registry, so it is
// owner-thread only; other threads may store and copy weak_refs.
// The domain must outlive every weak_ref that is still locked or expired-checked.
template <class T>
class weak_ref
{
public:
    using element_type = T;

    weak_ref() noexcept = default;
    weak_ref(std::nullptr_t) noexcept {}

    template <class U>
        requires detail::ref_convertible<U, T>
    weak_ref(const root_ref<U>& r) noexcept
    {
        set(detail::ref_access::header(r), detail::ref_access::raw(r));
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    weak_ref(const trace_ref<U>& r) noexcept
    {
        set(detail::ref_access::header(r), detail::ref_access::raw(r));
    }

    template <class U>
        requires detail::ref_convertible<U, T>
    weak_ref(const weak_ref<U>& w) noexcept
        : domain_(w.domain_), slot_(w.slot_), generation_(w.generation_), ptr_(w.ptr_)
    {
    }

    [[nodiscard]] root_ref<T> lock() const
    {
        if (!domain_)
            return {};
        detail::object_header* h = detail::domain_access::resolve_weak(*domain_, slot_, generation_);
        if (!h)
            return {};
        return detail::ref_access::make_root<T>(h, ptr_);
    }

    // True when promotion would fail. Owner thread only.
    [[nodiscard]] bool expired() const
    {
        return !domain_ || detail::domain_access::resolve_weak(*domain_, slot_, generation_) == nullptr;
    }

    void reset() noexcept { *this = weak_ref(); }

private:
    template <class> friend class weak_ref;

    void set(detail::object_header* h, T* p) noexcept
    {
        if (!h)
            return;
        domain_ = h->owner;
        slot_ = h->slot;
        generation_ = h->generation;
        ptr_ = p;
    }

    domain* domain_ = nullptr;
    std::uint32_t slot_ = 0;
    std::uint32_t generation_ = 0;
    T* ptr_ = nullptr; // never dereferenced unless promotion succeeds
};
} // namespace gc
