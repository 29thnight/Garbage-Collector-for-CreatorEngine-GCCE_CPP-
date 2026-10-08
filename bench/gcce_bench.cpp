// Compares the size-class pool heap with the global operator new heap on the
// same collector. Run one process per heap so memory figures do not mix:
//
//     gcce_bench class  [scale]   size-class pools (default heap)
//     gcce_bench type   [scale]   one pool per type
//     gcce_bench system [scale]   global operator new
//
// scale (default 1.0) multiplies object counts.

#include <gc/gc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#    include <psapi.h>
#else
#    include <unistd.h>
#endif

namespace
{
using clock_type = std::chrono::steady_clock;

double ms_since(clock_type::time_point start)
{
    return std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
}

std::size_t resident_bytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
#else
    long pages = 0;
    long resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r"))
    {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

double mib(std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

// ------------------------------------------------------------ object mix

struct bench_node : gc::managed
{
    gc::trace_ref<bench_node> next;
    gc::trace_ref<bench_node> other;
    void gc_trace(gc::tracer& t) const
    {
        t.visit(next);
        t.visit(other);
    }
};

template <std::size_t Payload>
struct sized_node final : bench_node
{
    unsigned char payload[Payload];
};

using factory = gc::root_ref<bench_node> (*)(gc::domain&);

template <std::size_t N>
gc::root_ref<bench_node> make_sized(gc::domain& d)
{
    return gc::make<sized_node<N>>(d);
}

// A game-like mix: many small components, fewer large ones.
constexpr std::array<factory, 6> factories{&make_sized<16>,  &make_sized<48>,  &make_sized<112>,
                                           &make_sized<240>, &make_sized<496>, &make_sized<1008>};
constexpr std::array<int, 6> weights{40, 25, 15, 10, 7, 3};

// Many distinct types, as in an engine with hundreds of component types:
// 200 types, sizes spread over the same range, object counts per type
// skewed (a few common types, a long tail of rare ones).
constexpr std::size_t type_count = 200;

template <std::size_t I>
gc::root_ref<bench_node> make_typed(gc::domain& d)
{
    constexpr std::size_t payload = 16 + (I * 37) % 1000;
    struct typed final : bench_node
    {
        unsigned char payload_bytes[payload];
    };
    return gc::make<typed>(d);
}

template <std::size_t... I>
constexpr std::array<factory, sizeof...(I)> typed_factories(std::index_sequence<I...>)
{
    return {&make_typed<I>...};
}

constexpr auto many_types = typed_factories(std::make_index_sequence<type_count>{});

class mix
{
public:
    explicit mix(std::uint32_t seed) : rng_(seed), dist_(weights.begin(), weights.end()) {}
    gc::root_ref<bench_node> make(gc::domain& d) { return factories[dist_(rng_)](d); }
    std::mt19937& rng() { return rng_; }

private:
    std::mt19937 rng_;
    std::discrete_distribution<int> dist_;
};

double percentile(std::vector<double> v, double p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[i];
}

double median(std::vector<double> v) { return percentile(std::move(v), 0.5); }

struct options
{
    gc::heap_kind heap;
    const char* name;
    double scale;
    bool only_types;
};

std::size_t scaled(const options& o, std::size_t n)
{
    return std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(n) * o.scale));
}

// ----------------------------------------------------------- scenarios

// Builds a live set, frees 90% at random, refills half of it. Reports
// process memory at each point; runs first, in a fresh process.
void memory_scenario(const options& o)
{
    const std::size_t n = scaled(o, 1000000);
    const std::size_t base = resident_bytes();
    gc::domain d(gc::domain_config{o.heap});
    mix m(1);

    std::vector<gc::root_ref<bench_node>> live;
    live.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        live.push_back(m.make(d));
    const std::size_t full = resident_bytes();
    const std::size_t live_bytes_full = d.stats().live_bytes;

    std::shuffle(live.begin(), live.end(), m.rng());
    live.resize(n / 10);
    d.collect_full();
    const std::size_t after_free = resident_bytes();

    for (std::size_t i = 0; i < n * 4 / 10; ++i)
        live.push_back(m.make(d));
    const std::size_t refilled = resident_bytes();
    const auto st = d.stats();

    std::printf("memory      objects=%zu  gc_live_full=%.1f MiB\n", n, mib(live_bytes_full));
    std::printf("memory      rss_full=%.1f MiB  rss_after_90pct_free=%.1f MiB  rss_refilled_50pct=%.1f MiB\n",
                mib(full - base), mib(after_free - base), mib(refilled - base));
    std::printf("memory      committed=%.1f MiB  pages=%zu  chunks=%zu  gc_live=%.1f MiB\n",
                mib(st.heap_committed_bytes), st.heap_pages, st.heap_chunks, mib(st.live_bytes));
}

// Allocation and reclamation cost per object.
void throughput_scenario(const options& o)
{
    const std::size_t n = scaled(o, 1000000);
    std::vector<double> alloc_ns;
    std::vector<double> reclaim_ns;
    gc::domain d(gc::domain_config{o.heap});
    mix m(2);
    for (int rep = 0; rep < 7; ++rep)
    {
        auto t0 = clock_type::now();
        for (std::size_t i = 0; i < n; ++i)
            (void)m.make(d);
        alloc_ns.push_back(ms_since(t0) * 1e6 / static_cast<double>(n));

        t0 = clock_type::now();
        d.collect_full();
        reclaim_ns.push_back(ms_since(t0) * 1e6 / static_cast<double>(n));
    }
    std::printf("throughput  alloc=%.1f ns/obj  reclaim=%.1f ns/obj  (median of 7, %zu objects)\n", median(alloc_ns),
                median(reclaim_ns), n);
}

// Marking speed over a long-lived graph that was built while garbage was
// being allocated and freed around it, as in a running game.
void trace_scenario(const options& o)
{
    const std::size_t n = scaled(o, 500000);
    gc::domain d(gc::domain_config{o.heap});
    mix m(3);

    gc::root_ref<bench_node> head = m.make(d);
    bench_node* tail = head.get();
    std::vector<bench_node*> nodes{tail};
    for (std::size_t i = 1; i < n; ++i)
    {
        for (int g = 0; g < 2; ++g)
            (void)m.make(d); // garbage interleaved with the live graph
        tail->next = m.make(d);
        tail = tail->next.get();
        nodes.push_back(tail);
        if (i % 100000 == 0)
            d.collect_full();
    }
    for (std::size_t i = 0; i < n; ++i) // random cross links
        nodes[i]->other = nodes[m.rng()() % n]->root_from_this();
    d.collect_full();

    std::vector<double> mark_ms;
    for (int rep = 0; rep < 7; ++rep)
    {
        auto r = d.collect_full();
        mark_ms.push_back(std::chrono::duration<double, std::milli>(r.mark_time).count());
    }
    std::printf("trace       mark=%.2f ms for %zu live objects  (%.1f ns/obj, median of 7)\n", median(mark_ms), n,
                median(mark_ms) * 1e6 / static_cast<double>(n));
}

// Frame loop: a stable live set with steady churn and an incremental step
// each frame. Reports frame time distribution.
void churn_scenario(const options& o)
{
    const std::size_t live_target = scaled(o, 200000);
    const std::size_t per_frame = scaled(o, 3000);
    const int frames = 600;
    gc::domain d(gc::domain_config{o.heap});
    mix m(4);

    std::vector<gc::root_ref<bench_node>> live;
    for (std::size_t i = 0; i < live_target; ++i)
        live.push_back(m.make(d));
    // Pace collection against a limit of twice the live set.
    d.set_pacing({d.stats().live_bytes * 2, 0.6, 8.0});

    std::vector<double> frame_ms;
    std::vector<double> gc_ms;
    const auto total_start = clock_type::now();
    for (int f = 0; f < frames; ++f)
    {
        const auto frame_start = clock_type::now();
        for (std::size_t i = 0; i < per_frame; ++i)
        {
            auto obj = m.make(d);
            if (i % 3 == 0)
                live[m.rng()() % live.size()] = std::move(obj); // replace a live object
        }
        const auto gc_start = clock_type::now();
        d.collect_step({std::chrono::microseconds(500), 64});
        gc_ms.push_back(ms_since(gc_start));
        frame_ms.push_back(ms_since(frame_start));
    }
    const double total = ms_since(total_start);
    const auto st = d.stats();
    std::printf("churn       frames=%d total=%.0f ms  frame p50=%.3f p99=%.3f max=%.3f ms  gc-step p99=%.3f ms  "
                "cycles=%llu  peak_gc_live=%.1f MiB  committed=%.1f MiB  rss=%.1f MiB\n",
                frames, total, percentile(frame_ms, 0.5), percentile(frame_ms, 0.99),
                *std::max_element(frame_ms.begin(), frame_ms.end()), percentile(gc_ms, 0.99),
                static_cast<unsigned long long>(st.cycles_completed), mib(st.peak_live_bytes),
                mib(st.heap_committed_bytes), mib(resident_bytes()));
}
// 200 types with skewed counts: memory per heap and allocation speed.
void many_types_scenario(const options& o)
{
    const std::size_t total = scaled(o, 300000);
    const std::size_t base = resident_bytes();
    gc::domain d(gc::domain_config{o.heap});
    std::mt19937 rng(5);
    // Zipf-like weights: type k gets weight 1/(k+1).
    std::vector<double> w(type_count);
    for (std::size_t k = 0; k < type_count; ++k)
        w[k] = 1.0 / static_cast<double>(k + 1);
    std::discrete_distribution<std::size_t> pick(w.begin(), w.end());

    std::vector<gc::root_ref<bench_node>> live;
    live.reserve(total);
    const auto t0 = clock_type::now();
    for (std::size_t i = 0; i < total; ++i)
        live.push_back(many_types[pick(rng)](d));
    const double alloc_ns = ms_since(t0) * 1e6 / static_cast<double>(total);
    const auto st = d.stats();
    std::printf("many-types  types=%zu objects=%zu  alloc=%.1f ns/obj  gc_live=%.1f MiB  committed=%.1f MiB  "
                "pages=%zu  rss=%.1f MiB\n",
                type_count, total, alloc_ns, mib(st.live_bytes), mib(st.heap_committed_bytes), st.heap_pages,
                mib(resident_bytes() - base));
}
} // namespace

int main(int argc, char** argv)
{
    options o{gc::heap_kind::size_class_pools, "class", 1.0, false};
    if (argc > 1 && std::strcmp(argv[1], "type") == 0)
        o = {gc::heap_kind::per_type_pools, "type", 1.0, false};
    if (argc > 1 && std::strcmp(argv[1], "system") == 0)
        o = {gc::heap_kind::system, "system", 1.0, false};
    if (argc > 2)
        o.scale = std::atof(argv[2]);
    if (argc > 3 && std::strcmp(argv[3], "many-types") == 0)
        o.only_types = true;

    std::printf("== heap: %s  scale: %.2f\n", o.name, o.scale);
    if (o.only_types)
    {
        many_types_scenario(o); // in its own process: memory figures stay separate
        return 0;
    }
    memory_scenario(o); // first, while the process heap is fresh
    throughput_scenario(o);
    trace_scenario(o);
    churn_scenario(o);
    return 0;
}
