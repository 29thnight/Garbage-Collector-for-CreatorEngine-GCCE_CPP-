#pragma once

#include "config.hpp"
#include "detail/header.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4251) // std::unique_ptr member in exported class
#endif

namespace gc
{
enum class phase : std::uint8_t
{
    idle,
    marking,
    sweeping
};

enum class violation_kind : std::uint8_t
{
    wrong_thread,                       // registry or graph change off the owner thread
    nested_collection,                  // collection started from a constructor, tracer or destructor
    allocation_during_trace,            // make() called from a trace function
    store_of_condemned_object,          // strong reference created to an object being reclaimed
    unreachable_with_cleanup_obligation,// engine cleanup unfinished but no longer reachable
    roots_remaining_at_shutdown,
    objects_remaining_at_shutdown
};

[[nodiscard]] GC_API const char* to_string(violation_kind kind) noexcept;

struct violation
{
    violation_kind kind;
    const char* type_name; // type of the object involved, or nullptr
};

// The default handler prints the violation and aborts. A replacement handler
// that returns lets the operation continue; that is meant for tests and tools.
using violation_handler = std::function<void(const violation&)>;

struct collect_result
{
    bool completed = false; // the cycle reclaimed its candidates
    bool aborted = false;   // the cycle stopped before reclaiming (protocol violation)
    bool refused = false;   // the cycle was not started
    std::size_t marked = 0;
    std::size_t edges_visited = 0;
    std::size_t reclaimed = 0;
    std::size_t reclaimed_bytes = 0;
    std::size_t violations = 0;
};

struct statistics
{
    phase current_phase = phase::idle;
    std::uint64_t cycles_completed = 0;
    std::uint64_t cycles_aborted = 0;
    std::size_t live_objects = 0;
    std::size_t live_bytes = 0;
    std::size_t roots = 0;
    std::size_t bytes_since_last_cycle = 0;
    std::size_t allocation_threshold = 0;
    bool collection_requested = false;
    collect_result last;
};

namespace detail
{
struct domain_access;
}

// One GC domain per engine instance. Scene graphs of every Scene in the engine
// share it so that cross-Scene references are traced.
//
// M1: collect_full() is the synchronous baseline collector. The reference
// types already route every non-null store through the insertion barrier so
// that M2 can split marking without changing their contract.
class GC_API domain
{
public:
    domain();
    ~domain();

    domain(const domain&) = delete;
    domain& operator=(const domain&) = delete;

    // Runs a whole cycle. Only call from an engine-controlled safe point on
    // the owner thread, never from constructors, tracers or destructors.
    collect_result collect_full();

    // Records a request; the engine loop decides when to act on it.
    void request_collection() noexcept;
    [[nodiscard]] bool collection_requested() const noexcept;

    // New GC bytes after which a collection is requested automatically.
    void set_allocation_threshold(std::size_t bytes) noexcept;

    [[nodiscard]] statistics stats() const;
    [[nodiscard]] phase current_phase() const noexcept { return phase_; }

    void set_violation_handler(violation_handler handler);

    // The owner thread defaults to the constructing thread.
    void bind_to_current_thread() noexcept;

private:
    friend struct detail::domain_access;
    friend class tracer;

    struct pending_allocation
    {
        detail::object_header* header;
    };

    pending_allocation begin_allocation(const detail::type_descriptor& type);
    void abort_allocation(pending_allocation& pending) noexcept;
    detail::object_header* publish(pending_allocation& pending) noexcept;

    void on_store(detail::object_header* target);
    void link_root(detail::root_node* node);
    void unlink_root(detail::root_node* node) noexcept;
    [[nodiscard]] detail::object_header* resolve_weak(std::uint32_t slot, std::uint32_t generation);

    bool advance_lifecycle(detail::object_header* header, lifecycle_state next);
    bool begin_cleanup_obligation(detail::object_header* header);

    void shade(detail::object_header* header);
    void check_thread() noexcept;
    void report(violation_kind kind, const detail::object_header* header) noexcept;
    void reclaim(detail::object_header* header) noexcept;

    struct impl;
    std::unique_ptr<impl> impl_;
    phase phase_ = phase::idle;
};

namespace detail
{
// Internal bridge used by the reference templates and make().
struct domain_access
{
    static domain::pending_allocation begin_allocation(domain& d, const type_descriptor& type)
    {
        return d.begin_allocation(type);
    }
    static void abort_allocation(domain& d, domain::pending_allocation& p) noexcept { d.abort_allocation(p); }
    static object_header* publish(domain& d, domain::pending_allocation& p) noexcept { return d.publish(p); }

    static void on_store(object_header* target) { target->owner->on_store(target); }
    static void link_root(root_node* node) { node->header->owner->link_root(node); }
    static void unlink_root(root_node* node) noexcept { node->header->owner->unlink_root(node); }
    static object_header* resolve_weak(domain& d, std::uint32_t slot, std::uint32_t generation)
    {
        return d.resolve_weak(slot, generation);
    }

    static bool advance_lifecycle(object_header* h, lifecycle_state next)
    {
        return h->owner->advance_lifecycle(h, next);
    }
    static bool begin_cleanup_obligation(object_header* h) { return h->owner->begin_cleanup_obligation(h); }
};
} // namespace detail
} // namespace gc

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif
