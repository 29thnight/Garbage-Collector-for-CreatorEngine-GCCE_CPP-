#pragma once

#include <gc/gc.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <mutex>
#include <vector>

namespace gctest
{
// Records violations instead of aborting so a test can assert on them.
// Tests without a recorder keep the default handler, so any unexpected
// violation aborts the test binary.
class violation_recorder
{
public:
    explicit violation_recorder(gc::domain& d) : domain_(d)
    {
        d.set_violation_handler([this](const gc::violation& v) {
            std::lock_guard lock(mutex_);
            kinds_.push_back(v.kind);
        });
    }

    // The domain may outlive the recorder; later violations are ignored.
    ~violation_recorder()
    {
        domain_.set_violation_handler([](const gc::violation&) {});
    }

    violation_recorder(const violation_recorder&) = delete;
    violation_recorder& operator=(const violation_recorder&) = delete;

    std::size_t count(gc::violation_kind kind)
    {
        std::lock_guard lock(mutex_);
        std::size_t n = 0;
        for (auto k : kinds_)
            n += k == kind;
        return n;
    }

    std::size_t total()
    {
        std::lock_guard lock(mutex_);
        return kinds_.size();
    }

private:
    gc::domain& domain_;
    std::mutex mutex_;
    std::vector<gc::violation_kind> kinds_;
};

// General purpose graph node.
struct node : gc::managed
{
    static inline int alive = 0;

    int id;
    gc::trace_ref<node> next;
    std::vector<gc::trace_ref<node>> children;

    explicit node(int id_ = 0) : id(id_) { ++alive; }
    node(const node&) = delete;
    node& operator=(const node&) = delete;
    ~node() { --alive; }

    void gc_trace(gc::tracer& t) const
    {
        t.visit(next);
        t.visit(children);
    }
};

inline std::size_t live(const gc::domain& d) { return d.stats().live_objects; }

// One unit of work per step: makes slice boundaries deterministic.
inline const gc::step_budget one_unit{gc::duration::zero(), 1};

// Drives the cycle in progress to its end with one-unit steps.
inline void finish_cycle(gc::domain& d)
{
    while (d.current_phase() != gc::phase::idle)
        d.collect_step(one_unit);
}

// Starts a cycle and steps until the reclaim decision has been taken.
inline void step_until_finalized(gc::domain& d)
{
    d.request_collection();
    for (;;)
    {
        auto r = d.collect_step(one_unit);
        if (r.finalized || r.cycle_finished)
            return;
    }
}

// Builds root -> c1 -> ... -> cN through `next`. Element 0 is the root.
// Raw pointers are only read by the tests, never stored into references.
inline std::vector<node*> make_chain(gc::domain& d, gc::root_ref<node>& root, int length)
{
    root = gc::make<node>(d, 0);
    std::vector<node*> chain{root.get()};
    for (int i = 1; i <= length; ++i)
    {
        chain.back()->next = gc::make<node>(d, i);
        chain.push_back(chain.back()->next.get());
    }
    return chain;
}
} // namespace gctest
