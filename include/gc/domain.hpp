#pragma once

#include "config.hpp"
#include "detail/header.hpp"

#include <chrono>
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
// marking also covers the incremental verification that precedes the
// reclaim decision; the barrier is active throughout. The decision itself and
// the switch to sweeping happen inside a single step and are not observable.
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

using duration = std::chrono::nanoseconds;

// Soft budget for one collect_step. Work is split at object boundaries, so a
// single large object (a huge container, a slow destructor) or the finalize
// transition can overrun it; overruns are recorded, not prevented.
struct step_budget
{
    duration time = std::chrono::microseconds(250);
    // Work units done even when the time is already spent, so that a busy
    // frame cannot stall a cycle forever.
    std::size_t min_units = 16;
};

struct step_result
{
    phase before = phase::idle;
    phase after = phase::idle;
    bool cycle_started = false;
    bool cycle_finished = false; // completed or aborted in this step
    bool finalized = false;      // the reclaim decision was taken in this step
    bool over_budget = false;
    bool refused = false;
    std::size_t units = 0;
    duration elapsed{};
    duration longest_unit{};
    duration finalize_time{};
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
    collect_result last;    // last finished cycle
    collect_result current; // cycle in progress

    // Incremental work (cumulative).
    std::uint64_t steps = 0;
    std::uint64_t steps_over_budget = 0;
    std::uint64_t barrier_stores_during_mark = 0; // non-null stores while marking
    std::uint64_t barrier_shades = 0;             // stores that added gray work
    duration worst_step{};
    duration worst_unit{};     // longest indivisible unit (one trace or destructor)
    duration worst_finalize{}; // longest reclaim-decision transition
    duration last_cycle_wall{}; // start to end of the last finished cycle
    std::uint64_t last_cycle_steps = 0;
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

    // Advances the collector within a soft budget. Starts a cycle when one
    // was requested or the maximum interval elapsed; otherwise does nothing.
    // Owner thread, engine safe points only; never from constructors,
    // tracers or destructors.
    step_result collect_step(const step_budget& budget = {});

    // Finishes a cycle in progress, then runs a complete new cycle, for
    // loading and shutdown boundaries. The result describes the new cycle.
    collect_result collect_full();

    // Records a request; the engine loop decides when to act on it.
    void request_collection() noexcept;
    [[nodiscard]] bool collection_requested() const noexcept;

    // New GC bytes after which a collection is requested automatically.
    void set_allocation_threshold(std::size_t bytes) noexcept;

    // collect_step starts a cycle when this much time has passed since the
    // last one ended, even without new allocation. Zero disables it.
    void set_max_cycle_interval(duration interval) noexcept;

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
    [[nodiscard]] bool is_marked(const detail::object_header* header) const noexcept;
    [[nodiscard]] bool is_condemned(const detail::object_header* header) const noexcept;
    void check_thread() noexcept;
    void report(violation_kind kind, const detail::object_header* header) noexcept;
    void reclaim(detail::object_header* header) noexcept;

    bool enter_collection(bool& refused) noexcept;
    void start_cycle();
    // Does one unit of work. Returns false when the cycle ended.
    bool do_unit(tracer& t, step_result& r);
    void finalize(tracer& t, step_result& r);
    void finish_cycle(bool completed);
    void run_to_cycle_end();

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
