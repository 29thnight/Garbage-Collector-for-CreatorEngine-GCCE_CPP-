#include "support.hpp"

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

using gctest::live;
using gctest::node;

#if GC_DEBUG_CHECKS
// Root registration and reference stores belong to the owner thread.
TEST(Threading, RegistryChangesOffTheOwnerThreadAreReported)
{
    gc::domain d;
    gctest::violation_recorder rec(d);
    auto a = gc::make<node>(d, 1);

    std::thread worker([&] {
        gc::root_ref<node> r = a; // store + link
    });                           // unlink
    worker.join();
    EXPECT_EQ(rec.count(gc::violation_kind::wrong_thread), 3u);

    std::thread allocator([&] { (void)gc::make<node>(d, 2); });
    allocator.join();
    EXPECT_GE(rec.count(gc::violation_kind::wrong_thread), 4u);
}

TEST(Threading, OwnershipCanMoveToAnotherThread)
{
    gc::domain d; // default handler: any wrong-thread access would abort
    auto a = gc::make<node>(d, 1);
    gc::weak_ref<node> w = a;
    a = nullptr;

    std::thread game_thread([&] {
        d.bind_to_current_thread();
        auto b = w.lock();
        b->next = gc::make<node>(d, 2);
        d.collect_full();
        EXPECT_EQ(live(d), 2u);
        b = nullptr;
        d.collect_full();
        EXPECT_EQ(live(d), 0u);
    });
    game_thread.join();
    d.bind_to_current_thread();
}
#endif

// Workers may read objects that the owner thread keeps alive with roots
// for the duration of the work, while the owner thread keeps collecting.
TEST(Threading, WorkersReadObjectsProtectedByOwnerRoots)
{
    gc::domain d;
    std::vector<gc::root_ref<node>> pinned;
    for (int i = 0; i < 64; ++i)
    {
        pinned.push_back(gc::make<node>(d, i));
        for (int j = 0; j < 16; ++j)
            pinned.back()->children.push_back(gc::make<node>(d, i * 100 + j));
    }

    // Raw pointers are handed out only after the roots exist.
    std::vector<const node*> work;
    for (const auto& p : pinned)
        work.push_back(p.get());

    std::atomic<long long> sum{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
        workers.emplace_back([&, t] {
            long long local = 0;
            for (std::size_t i = static_cast<std::size_t>(t); i < work.size(); i += 4)
                for (const auto& c : work[i]->children)
                    local += c->id;
            sum += local;
        });

    // Meanwhile the owner thread creates and collects garbage.
    for (int round = 0; round < 20; ++round)
    {
        for (int i = 0; i < 100; ++i)
            (void)gc::make<node>(d, -1);
        d.collect_full();
    }
    for (auto& w : workers)
        w.join();

    long long expected = 0;
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 16; ++j)
            expected += i * 100 + j;
    EXPECT_EQ(sum.load(), expected);

    pinned.clear(); // released only after the workers finished
    d.collect_full();
    EXPECT_EQ(live(d), 0u);
}

TEST(Threading, IndependentDomainsOnSeparateThreads)
{
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            gc::domain d;
            auto root = gc::make<node>(d, 0);
            for (int i = 0; i < 1000; ++i)
                root->children.push_back(gc::make<node>(d, i));
            for (int i = 0; i < 1000; ++i)
                (void)gc::make<node>(d, i);
            if (d.collect_full().reclaimed == 1000)
                ++ok;
        });
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(ok.load(), 4);
}
