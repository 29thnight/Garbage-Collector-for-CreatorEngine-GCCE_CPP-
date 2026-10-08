#pragma once

#include "refs.hpp"

#include <cstddef>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace gc
{
namespace detail
{
template <class T> struct is_trace_ref : std::false_type {};
template <class T> struct is_trace_ref<trace_ref<T>> : std::true_type {};

template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};

template <class T> struct is_pair : std::false_type {};
template <class A, class B> struct is_pair<std::pair<A, B>> : std::true_type {};

template <class T>
concept has_gc_trace = requires(const T& value, tracer& t) { value.gc_trace(t); };

// A type is traceable when it is a trace_ref, declares gc_trace, or is an
// optional, pair or range that contains a traceable type.
template <class T>
consteval bool traceable()
{
    using U = std::remove_cvref_t<T>;
    if constexpr (is_trace_ref<U>::value || has_gc_trace<U>)
        return true;
    else if constexpr (is_optional<U>::value)
        return traceable<typename U::value_type>();
    else if constexpr (is_pair<U>::value)
        return traceable<typename U::first_type>() || traceable<typename U::second_type>();
    else if constexpr (std::ranges::range<U>)
        return traceable<std::ranges::range_value_t<U>>();
    else
        return false;
}
} // namespace detail

// Passed to gc_trace. A traced type lists every strong reference it holds,
// including references in base classes, nested values and containers, and
// whether or not they are serialized:
//
//     void gc_trace(gc::tracer& t) const
//     {
//         Base::gc_trace(t);
//         t.visit(m_owner);
//         t.visit(m_children);
//     }
//
// gc_trace runs inside a collection. It must not allocate, collect, change
// references or call into game code.
class tracer
{
public:
    tracer(const tracer&) = delete;
    tracer& operator=(const tracer&) = delete;

    template <class T>
    void visit(const T& value)
    {
        using U = std::remove_cvref_t<T>;
        static_assert(detail::traceable<U>(),
                      "gc::tracer::visit: the type holds no trace_ref. Visit trace_ref, types with gc_trace, "
                      "or optionals, pairs and containers of them. weak_ref is never traced.");

        if constexpr (detail::is_trace_ref<U>::value)
            mark(detail::ref_access::header(value));
        else if constexpr (detail::has_gc_trace<U>)
            value.gc_trace(*this);
        else if constexpr (detail::is_optional<U>::value)
        {
            if (value)
                visit(*value);
        }
        else if constexpr (detail::is_pair<U>::value)
        {
            if constexpr (detail::traceable<typename U::first_type>())
                visit(value.first);
            if constexpr (detail::traceable<typename U::second_type>())
                visit(value.second);
        }
        else
        {
            for (const auto& element : value)
                visit(element);
        }
    }

private:
    friend class domain;

    explicit tracer(domain& d) noexcept : domain_(d) {}

    void mark(detail::object_header* header)
    {
        if (!header)
            return;
        ++edges_;
        domain_.shade(header);
    }

    domain& domain_;
    std::size_t edges_ = 0;
};
} // namespace gc
