#include "gc/domain.hpp"
#include "gc/tracer.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace gc
{
namespace
{
struct slot
{
    detail::object_header* header = nullptr;
    // Starts at 1 so a default weak_ref (generation 0) never resolves.
    std::uint32_t generation = 1;
};

constexpr std::size_t default_allocation_threshold = std::size_t{4} * 1024 * 1024;
constexpr std::uint32_t max_generation = std::numeric_limits<std::uint32_t>::max();

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
    std::vector<detail::object_header*> gray;

    std::size_t live_objects = 0;
    std::size_t live_bytes = 0;
    std::size_t bytes_since_last_cycle = 0;
    std::size_t allocation_threshold = default_allocation_threshold;
    std::uint64_t next_serial = 0;
    std::uint64_t cycles_completed = 0;
    std::uint64_t cycles_aborted = 0;

    unsigned construct_depth = 0;
    bool collecting = false;
    bool tracing = false;
    bool requested = false;

    violation_handler handler = &default_violation_handler;
    collect_result last;

    impl() noexcept { roots.prev = roots.next = &roots; }
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
            s.gray.reserve(s.gray.size() + 1);
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

    // An object completed during marking is scanned in this cycle (gray capacity
    // was reserved in begin_allocation).
    if (phase_ == phase::marking)
        shade(h);
    return h;
}

// ------------------------------------------------------------ references

void domain::on_store(detail::object_header* target)
{
    check_thread();
    if (target->reclaim == detail::reclaim_state::condemned)
        report(violation_kind::store_of_condemned_object, target);
    // Insertion barrier: a non-null store during marking shades the target.
    if (phase_ == phase::marking)
        shade(target);
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
    if (sl.header->reclaim != detail::reclaim_state::live)
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
    return true;
}

// ------------------------------------------------------------- collection

void domain::shade(detail::object_header* header)
{
    if (header->mark != detail::mark_color::white)
        return;
    header->mark = detail::mark_color::gray;
    impl_->gray.push_back(header);
}

void domain::reclaim(detail::object_header* h) noexcept
{
    impl& s = *impl_;
    h->type->destroy(h->object);

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

collect_result domain::collect_full()
{
    impl& s = *impl_;
    check_thread();

    collect_result result;
    if (s.collecting)
    {
        report(violation_kind::nested_collection, nullptr);
        result.refused = true;
        return result;
    }
    if (s.construct_depth != 0)
    {
        // Constructors may only request; the engine loop collects later.
        report(violation_kind::nested_collection, nullptr);
        s.requested = true;
        result.refused = true;
        return result;
    }
    s.collecting = true;

    // ---- Mark
    phase_ = phase::marking;
    for (slot& sl : s.slots)
        if (sl.header)
            sl.header->mark = detail::mark_color::white;

    // Every object enters the gray stack at most once per cycle.
    s.gray.clear();
    s.gray.reserve(s.live_objects);

    for (detail::root_node* n = s.roots.next; n != &s.roots; n = n->next)
        shade(n->header);

    tracer t(*this);
    s.tracing = true;
    while (!s.gray.empty())
    {
        detail::object_header* h = s.gray.back();
        s.gray.pop_back();
        h->mark = detail::mark_color::black;
        h->type->trace(h->object, t);
        ++result.marked;
    }
    s.tracing = false;
    result.edges_visited = t.edges_;

    // ---- Finalize: fix the candidate set and check engine invariants.
    std::vector<detail::object_header*> candidates;
    for (slot& sl : s.slots)
        if (sl.header && sl.header->mark == detail::mark_color::white)
            candidates.push_back(sl.header);

    for (detail::object_header* h : candidates)
    {
        if (h->cleanup_obligation)
        {
            report(violation_kind::unreachable_with_cleanup_obligation, h);
            ++result.violations;
        }
    }
    if (result.violations != 0)
    {
        result.aborted = true;
        ++s.cycles_aborted;
        phase_ = phase::idle;
        s.collecting = false;
        s.last = result;
        return result;
    }

    // ---- Sweep
    phase_ = phase::sweeping;
    // Condemn the whole set first so destructors see consistent state:
    // weak promotion of any candidate fails from here on.
    for (detail::object_header* h : candidates)
        h->reclaim = detail::reclaim_state::condemned;

    for (detail::object_header* h : candidates)
    {
        result.reclaimed_bytes += h->block_size;
        reclaim(h);
        ++result.reclaimed;
    }

    phase_ = phase::idle;
    s.collecting = false;
    s.requested = false;
    s.bytes_since_last_cycle = 0;
    ++s.cycles_completed;
    result.completed = true;
    s.last = result;
    return result;
}
} // namespace gc
