#pragma once

#include <gc/gc.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace gctest
{
struct test_case
{
    const char* name;
    void (*fn)();
};

std::vector<test_case>& registry();
[[noreturn]] void fail(const char* file, int line, const std::string& message);

struct registrar
{
    registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Records violations instead of aborting so tests can assert on them.
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

// General graph node used by most tests.
struct node
{
    static inline int alive = 0;

    int id;
    gc::trace_ref<node> next;
    std::vector<gc::trace_ref<node>> children;

    explicit node(int id_ = 0) : id(id_) { ++alive; }
    ~node() { --alive; }

    void gc_trace(gc::tracer& t) const
    {
        t.visit(next);
        t.visit(children);
    }
};
} // namespace gctest

#define GC_CONCAT_INNER(a, b) a##b
#define GC_CONCAT(a, b) GC_CONCAT_INNER(a, b)

#define GC_TEST(name)                                                                                        \
    static void name();                                                                                      \
    static const ::gctest::registrar GC_CONCAT(name, _registrar){#name, &name};                              \
    static void name()

#define CHECK(expr)                                                                                          \
    do                                                                                                       \
    {                                                                                                        \
        if (!(expr))                                                                                         \
            ::gctest::fail(__FILE__, __LINE__, #expr);                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                                                       \
    do                                                                                                       \
    {                                                                                                        \
        const auto& gc_a_ = (a);                                                                             \
        const auto& gc_b_ = (b);                                                                             \
        if (!(gc_a_ == gc_b_))                                                                               \
            ::gctest::fail(__FILE__, __LINE__,                                                               \
                           std::string(#a " == " #b " (") + std::to_string(gc_a_) + " vs " +                 \
                               std::to_string(gc_b_) + ")");                                                 \
    } while (0)
