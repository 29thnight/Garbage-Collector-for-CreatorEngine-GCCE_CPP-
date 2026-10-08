#include "gc/domain.hpp"
#include "gc/tracer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace gc
{
namespace
{
using clock = std::chrono::steady_clock;

struct slot
{
    detail::object_header* header = nullptr;
    // Starts at 1 so a default weak_ref (generation 0) never resolves.
    std::uint32_t generation = 1;
};

// Internal stages of a cycle. trace and verify are both phase::marking.
enum class stage : std::uint8_t
{
    idle,
    trace,  // drain the gray stack
    verify, // scan slots for unmarked objects with a cleanup obligation
    sweep
};

constexpr std::size_t default_allocation_threshold = std::size_t{4} * 1024 * 1024;
constexpr std::uint32_t max_generation = std::numeric_limits<std::uint32_t>::max();
// Slots looked at per verify or sweep unit when no destructor runs.
constexpr std::size_t slot_scan_batch = 256;

std::size_t round_up(std::size_t value, std::size_t align) noexcept
{
    return (value + align - 1) / align * align;
}

void default_violation_handler(const violation& v)
{
    std::fprintf(stderr, "gc: protocol violation: %s (type: %s)\n", to_string(v.kind),
                 v.type_name ? v.type_name : "-");
    std::abort();
}
} // namespace

const char* to_string(violation_kind kind) noexcept
{
    switch (kind)
    {
    case violation_kind::wrong_thread: return "wrong_thread";
    case violation_kind::nested_collection: return "nested_collection";
    case violation_kind::allocation_during_trace: return "allocation_during_trace";
    case violation_kind::store_of_condemned_object: return "store_of_condemned_object";
    case violation_kind::unreachable_with_cleanup_obligation: return "unreachable_with_cleanup_obligation";
    case violation_kind::roots_remaining_at_shutdown: return "roots_remaining_at_shutdown";
    case violation_kind::objects_remaining_at_shutdown: return "objects_remaining_at_shutdown";
    }
    return "unknown";
}

struct domain::impl
{
    std::thread::id owner = std::this_thread::get_id();

    detail::root_node roots; // circular list sentinel
    std::size_t root_count = 0;

    std::vector<slot> slots;
    std::vector<std::uint32_t> free_slots;

    // ---- cycle state
    stage current_stage = stage::idle;
    std::uint32_t epoch = 0;
    std::vector<detail::object_header*> gray;
    // Every object enters the gray stack at most once per cycle, so reserving
    // one entry per object that can be marked keeps pushes allocation-free.
    std::size_t gray_reserved = 0;
    std::size_t verify_cursor = 0;
    std::vector<detail::object_header*> suspects; // unmarked with obligation at verify time
    std::uint64_t sweep_cutoff = 0;               // objects with a later serial are not candidates
    std::size_t sweep_cursor = 0;
    std::size_t sweep_end = 0;
    clock::time_point cycle_start{};
    clock::time_point last_cycle_end = clock::now();
    std::uint64_t cycle_steps = 0;
    collect_result current;

    // ---- allocation and requests
    std::size_t live_objects = 0;
    std::size_t live_bytes = 0;
    std::size_t bytes_since_last_cycle = 0;
    std::size_t allocation_threshold = default_allocation_threshold;
    duration max_cycle_interval{};
    std::uint64_t next_serial = 0;
    unsigned construct_depth = 0;
    bool in_collection = false;
    bool tracing = false;
    bool requested = false;

    // ---- statistics
    std::uint64_t cycles_completed = 0;
    std::uint64_t cycles_aborted = 0;
    std::uint64_t steps = 0;
    std::uint64_t steps_over_budget = 0;
    std::uint64_t barrier_stores_during_mark = 0;
    std::uint64_t barrier_shades = 0;
    duration worst_step{};
    duration worst_unit{};
    duration worst_finalize{};
    duration last_cycle_wall{};
    std::uint64_t last_cycle_steps = 0;
    collect_result last;

    violation_handler handler = &default_violation_handler;

    impl() noexcept { roots.prev = roots.next = &roots; }

    void note_unit(duration d, step_result& r) noexcept
    {
        r.longest_unit = std::max(r.longest_unit, d);
        worst_unit = std::max(worst_unit, d);
    }
};

domain::domain() : impl_(std::make_unique<impl>()) {}

domain::~domain()
{
    impl& s = *impl_;
    if (s.root_count != 0)
    {
        // Unsupported: roots outliving the domain. Detach them so their
        // destructors become no-ops and leak the objects instead of freeing
        // memory that is still referenced.
        report(violation_kind::roots_remaining_at_shutdown, s.roots.next->header);
        for (detail::root_node* n = s.roots.next; n != &s.roots;)
        {
            detail::root_node* next = n->next;
            n->prev = n->next = nullptr;
            n->header = nullptr;
            n = next;
        }
        s.roots.prev = s.roots.next = &s.roots;
        s.root_count = 0;
        if (s.live_objects != 0)
            report(violation_kind::objects_remaining_at_shutdown, nullptr);
        return;
    }

    collect_full();
    if (s.live_objects != 0)
        report(violation_kind::objects_remaining_at_shutdown, nullptr);
}

void domain::report(violation_kind kind, const detail::object_header* header) noexcept
{
    const violation v{kind, header && header->type ? header->type->name : nullptr};
    impl_->handler(v);
}

void domain::check_thread() noexcept
{
#if GC_THREAD_CHECKS
    if (std::this_thread::get_id() != impl_->owner)
        report(violation_kind::wrong_thread, nullptr);
#endif
}

void domain::bind_to_current_thread() noexcept { impl_->owner = std::this_thread::get_id(); }

void domain::set_violation_handler(violation_handler handler)
{
    impl_->handler = handler ? std::move(handler) : violation_handler(&default_violation_handler);
}

void domain::request_collection() noexcept { impl_->requested = true; }

bool domain::collection_requested() const noexcept { return impl_->requested; }

void domain::set_allocation_threshold(std::size_t bytes) noexcept { impl_->allocation_threshold = bytes; }

void domain::set_max_cycle_interval(duration interval) noexcept { impl_->max_cycle_interval = interval; }

statistics domain::stats() const
{
    const impl& s = *impl_;
    statistics st;
    st.current_phase = phase_;
    st.cycles_completed = s.cycles_completed;
    st.cycles_aborted = s.cycles_aborted;
    st.live_objects = s.live_objects;
    st.live_bytes = s.live_bytes;
    st.roots = s.root_count;
    st.bytes_since_last_cycle = s.bytes_since_last_cycle;
    st.allocation_threshold = s.allocation_threshold;
    st.collection_requested = s.requested;
    st.last = s.last;
    st.current = s.current;
    st.steps = s.steps;
    st.steps_over_budget = s.steps_over_budget;
    st.barrier_stores_during_mark = s.barrier_stores_during_mark;
    st.barrier_shades = s.barrier_shades;
    st.worst_step = s.worst_step;
    st.worst_unit = s.worst_unit;
    st.worst_finalize = s.worst_finalize;
    st.last_cycle_wall = s.last_cycle_wall;
    st.last_cycle_steps = s.last_cycle_steps;
    return st;
}

// ---------------------------------------------------------------- allocation

domain::pending_allocation domain::begin_allocation(const detail::type_descriptor& type)
{
    impl& s = *impl_;
    check_thread();
    if (s.tracing)
        report(violation_kind::allocation_during_trace, nullptr);

    const std::size_t align = type.align > alignof(detail::object_header) ? type.align : alignof(detail::object_header);
    const std::size_t offset = round_up(sizeof(detail::object_header), type.align);
    const std::size_t size = offset + type.size;

    void* block = ::operator new(size, std::align_val_t{align});

    std::uint32_t index;
    try
    {
        if (!s.free_slots.empty())
        {
            index = s.free_slots.back();
            s.free_slots.pop_back();
        }
        else
        {
            s.slots.emplace_back();
            // Reserve so that releasing a slot during sweep never allocates.
            s.free_slots.reserve(s.slots.size());
            index = static_cast<std::uint32_t>(s.slots.size() - 1);
        }
        if (phase_ == phase::marking)
            s.gray.reserve(++s.gray_reserved);
    }
    catch (...)
    {
        ::operator delete(block, std::align_val_t{align});
        throw;
    }

    auto* header = ::new (block) detail::object_header{};
    header->owner = this;
    header->type = &type;
    header->object = static_cast<unsigned char*>(block) + offset;
    header->block_size = size;
    header->block_align = align;
    header->slot = index;

    ++s.construct_depth;
    return pending_allocation{header};
}

void domain::abort_allocation(pending_allocation& pending) noexcept
{
    impl& s = *impl_;
    detail::object_header* h = pending.header;
    --s.construct_depth;
    // The slot was never published; return it without bumping the generation.
    s.free_slots.push_back(h->slot);
    ::operator delete(static_cast<void*>(h), std::align_val_t{h->block_align});
    pending.header = nullptr;
}

detail::object_header* domain::publish(pending_allocation& pending) noexcept
{
    impl& s = *impl_;
    detail::object_header* h = pending.header;
    --s.construct_depth;

    slot& sl = s.slots[h->slot];
    h->generation = sl.generation;
    h->alloc_serial = ++s.next_serial;
    sl.header = h;

    ++s.live_objects;
    s.live_bytes += h->block_size;
    s.bytes_since_last_cycle += h->block_size;
    if (s.bytes_since_last_cycle >= s.allocation_threshold)
        s.requested = true;

    // An object completed during marking is scanned in this cycle. During
    // sweep its serial is past the cutoff, so it is never a candidate.
    if (phase_ == phase::marking)
        shade(h);
    return h;
}

// ------------------------------------------------------------ references

void domain::on_store(detail::object_header* target)
{
    impl& s = *impl_;
    check_thread();
    if (is_condemned(target))
        report(violation_kind::store_of_condemned_object, target);
    // Insertion barrier: a non-null store during marking shades the target.
    if (phase_ == phase::marking)
    {
        ++s.barrier_stores_during_mark;
        if (!is_marked(target))
        {
            ++s.barrier_shades;
            shade(target);
        }
    }
}

void domain::link_root(detail::root_node* node)
{
    impl& s = *impl_;
    check_thread();
    node->prev = &s.roots;
    node->next = s.roots.next;
    s.roots.next->prev = node;
    s.roots.next = node;
    ++s.root_count;
}

void domain::unlink_root(detail::root_node* node) noexcept
{
    check_thread();
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = nullptr;
    --impl_->root_count;
}

detail::object_header* domain::resolve_weak(std::uint32_t index, std::uint32_t generation)
{
    impl& s = *impl_;
    check_thread();
    if (index >= s.slots.size())
        return nullptr;
    const slot& sl = s.slots[index];
    if (!sl.header || sl.generation != generation)
        return nullptr;
    if (is_condemned(sl.header))
        return nullptr;
    return sl.header;
}

// ------------------------------------------------------------- lifecycle

bool domain::advance_lifecycle(detail::object_header* header, lifecycle_state next)
{
    check_thread();
    if (static_cast<int>(next) != static_cast<int>(header->lifecycle) + 1)
        return false;
    header->lifecycle = next;
    if (next == lifecycle_state::destroyed)
        header->cleanup_obligation = false;
    return true;
}

bool domain::begin_cleanup_obligation(detail::object_header* header)
{
    check_thread();
    if (header->lifecycle == lifecycle_state::destroyed)
        return false;
    header->cleanup_obligation = true;
    // An obligation that appears during marking must not be missed by the
    // verify scan, which may already have passed this slot: keep the object
    // for this cycle. If it is unreachable, the next cycle reports it.
    if (phase_ == phase::marking)
        shade(header);
    return true;
}

// ------------------------------------------------------------- collection

bool domain::is_marked(const detail::object_header* header) const noexcept
{
    return header->mark_epoch == impl_->epoch;
}

bool domain::is_condemned(const detail::object_header* header) const noexcept
{
    return phase_ == phase::sweeping && !is_marked(header) && header->alloc_serial <= impl_->sweep_cutoff;
}

void domain::shade(detail::object_header* header)
{
    if (is_marked(header))
        return;
    header->mark_epoch = impl_->epoch;
    impl_->gray.push_back(header); // capacity reserved, see gray_reserved
}

void domain::reclaim(detail::object_header* h) noexcept
{
    impl& s = *impl_;
    h->type->destroy(h->object);

    // Look the slot up after the destructor: it may have allocated.
    slot& sl = s.slots[h->slot];
    sl.header = nullptr;
    if (sl.generation == max_generation)
    {
        // Retire the slot: reusing it would let an old weak_ref match again.
    }
    else
    {
        ++sl.generation;
        s.free_slots.push_back(h->slot); // capacity reserved in begin_allocation
    }

    --s.live_objects;
    s.live_bytes -= h->block_size;
    ::operator delete(static_cast<void*>(h), std::align_val_t{h->block_align});
}

bool domain::enter_collection(bool& refused) noexcept
{
    impl& s = *impl_;
    check_thread();
    refused = true;
    if (s.in_collection)
    {
        report(violation_kind::nested_collection, nullptr);
        return false;
    }
    if (s.construct_depth != 0)
    {
        // Constructors may only request; the engine loop collects later.
        report(violation_kind::nested_collection, nullptr);
        s.requested = true;
        return false;
    }
    refused = false;
    s.in_collection = true;
    return true;
}

void domain::start_cycle()
{
    impl& s = *impl_;
    if (++s.epoch == 0)
    {
        // Epoch wrap: clear stale marks once so no object looks marked.
        s.epoch = 1;
        for (slot& sl : s.slots)
            if (sl.header)
                sl.header->mark_epoch = 0;
    }
    s.current = collect_result{};
    s.cycle_start = clock::now();
    s.cycle_steps = 0;
    s.requested = false;
    s.bytes_since_last_cycle = 0;
    s.suspects.clear();
    s.verify_cursor = 0;
    s.gray.clear();
    s.gray_reserved = s.live_objects;
    s.gray.reserve(s.gray_reserved);

    phase_ = phase::marking;
    s.current_stage = stage::trace;

    // Roots are scanned in one piece. Roots added later are shaded by the
    // barrier when they are linked, and removing a root needs no work.
    for (detail::root_node* n = s.roots.next; n != &s.roots; n = n->next)
        shade(n->header);
}

bool domain::do_unit(tracer& t, step_result& r)
{
    impl& s = *impl_;
    const auto t0 = clock::now();
    switch (s.current_stage)
    {
    case stage::trace:
        if (!s.gray.empty())
        {
            detail::object_header* h = s.gray.back();
            s.gray.pop_back();
            s.tracing = true;
            h->type->trace(h->object, t);
            s.tracing = false;
            ++s.current.marked;
            s.current.edges_visited += std::exchange(t.edges_, 0);
            s.note_unit(clock::now() - t0, r);
            return true;
        }
        s.current_stage = stage::verify;
        [[fallthrough]];

    case stage::verify:
        if (!s.gray.empty())
        {
            // The barrier added work since the trace stage ran dry.
            s.current_stage = stage::trace;
            return true;
        }
        if (s.verify_cursor < s.slots.size())
        {
            const std::size_t end = std::min(s.slots.size(), s.verify_cursor + slot_scan_batch);
            for (; s.verify_cursor < end; ++s.verify_cursor)
            {
                detail::object_header* h = s.slots[s.verify_cursor].header;
                if (h && h->cleanup_obligation && !is_marked(h))
                    s.suspects.push_back(h);
            }
            s.note_unit(clock::now() - t0, r);
            return true;
        }
        finalize(t, r);
        return s.current_stage != stage::idle;

    case stage::sweep:
        for (std::size_t scanned = 0; s.sweep_cursor < s.sweep_end && scanned < slot_scan_batch; ++scanned)
        {
            detail::object_header* h = s.slots[s.sweep_cursor++].header;
            if (h && is_condemned(h))
            {
                s.current.reclaimed_bytes += h->block_size;
                reclaim(h);
                ++s.current.reclaimed;
                break; // at most one destructor per unit
            }
        }
        s.note_unit(clock::now() - t0, r);
        if (s.sweep_cursor < s.sweep_end)
            return true;
        finish_cycle(true);
        return false;

    case stage::idle:
        break;
    }
    return false;
}

// The reclaim decision. Runs inside one step with no game code in between:
// drain all remaining marking work, recheck the suspects against the final
// mark state, then fix the allocation cutoff and enter sweep.
void domain::finalize(tracer& t, step_result& r)
{
    impl& s = *impl_;
    const auto t0 = clock::now();

    s.tracing = true;
    while (!s.gray.empty())
    {
        detail::object_header* h = s.gray.back();
        s.gray.pop_back();
        h->type->trace(h->object, t);
        ++s.current.marked;
    }
    s.tracing = false;
    s.current.edges_visited += std::exchange(t.edges_, 0);

    // Objects that gained an obligation after the verify scan were shaded,
    // so the suspects collected by the scan are the complete set.
    for (detail::object_header* h : s.suspects)
    {
        if (h->cleanup_obligation && !is_marked(h))
        {
            report(violation_kind::unreachable_with_cleanup_obligation, h);
            ++s.current.violations;
        }
    }
    s.suspects.clear();

    const duration elapsed = clock::now() - t0;
    r.finalized = true;
    r.finalize_time = elapsed;
    s.worst_finalize = std::max(s.worst_finalize, elapsed);
    s.note_unit(elapsed, r);

    if (s.current.violations != 0)
    {
        finish_cycle(false);
        return;
    }

    s.sweep_cutoff = s.next_serial;
    s.sweep_cursor = 0;
    s.sweep_end = s.slots.size();
    phase_ = phase::sweeping;
    s.current_stage = stage::sweep;
}

void domain::finish_cycle(bool completed)
{
    impl& s = *impl_;
    const auto now = clock::now();
    phase_ = phase::idle;
    s.current_stage = stage::idle;
    s.gray.clear();
    s.suspects.clear();

    s.current.completed = completed;
    s.current.aborted = !completed;
    if (completed)
        ++s.cycles_completed;
    else
        ++s.cycles_aborted;
    s.last = s.current;
    s.last_cycle_wall = now - s.cycle_start;
    s.last_cycle_steps = s.cycle_steps;
    s.last_cycle_end = now;
}

void domain::run_to_cycle_end()
{
    tracer t(*this);
    step_result r;
    ++impl_->cycle_steps;
    while (do_unit(t, r))
    {
    }
}

step_result domain::collect_step(const step_budget& budget)
{
    impl& s = *impl_;
    step_result r;
    r.before = phase_;

    bool refused = false;
    if (!enter_collection(refused))
    {
        r.refused = refused;
        r.after = phase_;
        return r;
    }

    const auto start = clock::now();
    if (phase_ == phase::idle)
    {
        const bool interval_due = s.max_cycle_interval > duration::zero() &&
                                  start - s.last_cycle_end >= s.max_cycle_interval;
        if (!s.requested && !interval_due)
        {
            s.in_collection = false;
            r.after = phase_;
            return r;
        }
        start_cycle();
        r.cycle_started = true;
    }

    ++s.steps;
    ++s.cycle_steps;
    tracer t(*this);
    for (;;)
    {
        const bool more = do_unit(t, r);
        ++r.units;
        if (!more)
        {
            r.cycle_finished = true;
            break;
        }
        if (r.units >= budget.min_units && clock::now() - start >= budget.time)
            break;
    }

    r.elapsed = clock::now() - start;
    r.over_budget = r.elapsed > budget.time;
    s.steps_over_budget += r.over_budget;
    s.worst_step = std::max(s.worst_step, r.elapsed);
    if (r.cycle_finished)
        s.last_cycle_steps = s.cycle_steps;

    s.in_collection = false;
    r.after = phase_;
    return r;
}

collect_result domain::collect_full()
{
    impl& s = *impl_;
    bool refused = false;
    if (!enter_collection(refused))
    {
        collect_result result;
        result.refused = true;
        return result;
    }

    if (phase_ != phase::idle)
        run_to_cycle_end();
    start_cycle();
    run_to_cycle_end();

    s.in_collection = false;
    return s.last;
}
} // namespace gc
