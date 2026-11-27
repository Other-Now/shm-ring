// Google Benchmark microbenchmarks: the single-core cost of each primitive,
// with no cache-line transfer involved (that is ipc_latency / ring_throughput).
//
//   * ring push+pop on one thread, per layout and for MPSC
//   * seqlock store / load
//   * object pool acquire+release vs new+delete
//   * the "zero-cost abstraction" check: the same push/pop loop called
//     directly, through a `template <Queue Q>` function, and through a virtual
//     interface whose target the compiler cannot see.

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "shmring/shmring.hpp"

using namespace shmring;

namespace {

struct Msg {
    std::uint64_t a, b, c;
};

template <class Q>
void BM_PushPop(benchmark::State& st) {
    auto q = std::make_unique<Q>();
    typename Q::value_type v{};
    std::uint64_t i = 0;
    for (auto _ : st) {
        v.a = i++;
        q->try_push(v);
        q->try_pop(v);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_PushPop<SpscRing<Msg, 1024, Layout::Packed>>)->Name("spsc_push_pop/packed");
BENCHMARK(BM_PushPop<SpscRing<Msg, 1024, Layout::Padded>>)->Name("spsc_push_pop/padded");
BENCHMARK(BM_PushPop<SpscRing<Msg, 1024, Layout::PaddedCached>>)->Name("spsc_push_pop/padded_cached");
BENCHMARK(BM_PushPop<MpscRing<Msg, 1024>>)->Name("mpsc_push_pop");

// Batch: fill 64 then drain 64 -- the cached-index layout should shine here
// because it re-reads the other index once per batch.
template <class Q>
void BM_Burst64(benchmark::State& st) {
    auto q = std::make_unique<Q>();
    Msg v{};
    for (auto _ : st) {
        for (int i = 0; i < 64; ++i) q->try_push(v);
        for (int i = 0; i < 64; ++i) q->try_pop(v);
        benchmark::DoNotOptimize(v);
    }
    st.SetItemsProcessed(st.iterations() * 64);
}
BENCHMARK(BM_Burst64<SpscRing<Msg, 1024, Layout::Padded>>)->Name("spsc_burst64/padded");
BENCHMARK(BM_Burst64<SpscRing<Msg, 1024, Layout::PaddedCached>>)->Name("spsc_burst64/padded_cached");
BENCHMARK(BM_Burst64<MpscRing<Msg, 1024>>)->Name("mpsc_burst64");

struct Tob4 {
    std::uint32_t bp, bs, ap, as;
};

void BM_SeqlockStore(benchmark::State& st) {
    auto sl = std::make_unique<Seqlock<Tob4>>();
    std::uint32_t i = 0;
    for (auto _ : st) {
        sl->store(Tob4{i, i, i, i});
        ++i;
    }
}
BENCHMARK(BM_SeqlockStore)->Name("seqlock_store");

void BM_SeqlockLoad(benchmark::State& st) {
    auto sl = std::make_unique<Seqlock<Tob4>>();
    sl->store(Tob4{1, 2, 3, 4});
    for (auto _ : st) benchmark::DoNotOptimize(sl->load());
}
BENCHMARK(BM_SeqlockLoad)->Name("seqlock_load");

// --------------------------------------------------------------- pool vs heap

struct Order {
    std::uint64_t id, sym;
    std::uint32_t px, qty;
    bool buy;
    Order(std::uint64_t i, std::uint64_t s) : id(i), sym(s), px(0), qty(0), buy(false) {}
};

void BM_PoolAcquireRelease(benchmark::State& st) {
    auto pool = std::make_unique<ObjectPool<Order, 1024>>();
    std::uint64_t i = 0;
    for (auto _ : st) {
        Order* o = pool->acquire(i++, 7);
        benchmark::DoNotOptimize(o);
        pool->release(o);
    }
}
BENCHMARK(BM_PoolAcquireRelease)->Name("pool_acquire_release");

void BM_NewDelete(benchmark::State& st) {
    std::uint64_t i = 0;
    for (auto _ : st) {
        Order* o = new Order(i++, 7);
        benchmark::DoNotOptimize(o);
        delete o;
    }
}
BENCHMARK(BM_NewDelete)->Name("new_delete");

// Pool vs heap when objects are held for a while (a realistic order lifetime):
// keep 256 live, recycle the oldest.
void BM_PoolChurn(benchmark::State& st) {
    auto pool = std::make_unique<ObjectPool<Order, 512>>();
    std::vector<Order*> live(256);
    for (std::size_t k = 0; k < live.size(); ++k) live[k] = pool->acquire(k, 7);
    std::size_t k = 0;
    for (auto _ : st) {
        pool->release(live[k]);
        live[k] = pool->acquire(k, 7);
        benchmark::DoNotOptimize(live[k]);
        k = (k + 1) & 255;
    }
    for (auto* o : live) pool->release(o);
}
BENCHMARK(BM_PoolChurn)->Name("pool_churn256");

void BM_HeapChurn(benchmark::State& st) {
    std::vector<Order*> live(256);
    for (std::size_t k = 0; k < live.size(); ++k) live[k] = new Order(k, 7);
    std::size_t k = 0;
    for (auto _ : st) {
        delete live[k];
        live[k] = new Order(k, 7);
        benchmark::DoNotOptimize(live[k]);
        k = (k + 1) & 255;
    }
    for (auto* o : live) delete o;
}
BENCHMARK(BM_HeapChurn)->Name("heap_churn256");

// --------------------------------------------------- zero-cost abstraction
//
// The claim being tested is not "an indirect call is slow" -- a perfectly
// predicted indirect call is nearly free. It is that code written against the
// Queue concept is instantiated for the concrete ring and so can be *inlined*
// into the caller, exactly like hand-written code, whereas a virtual interface
// cannot be inlined through. So: direct and concept versions are left to the
// optimiser; the virtual one has its dynamic type hidden.

using Q = SpscRing<Msg, 1024>;

template <Queue AnyQ>
inline void generic_push_pop(AnyQ& q, typename AnyQ::value_type& v) {
    q.try_push(v);
    q.try_pop(v);
}

struct IQueue {
    virtual ~IQueue() = default;
    virtual bool push(const Msg&) = 0;
    virtual bool pop(Msg&) = 0;
};
struct VirtualSpsc final : IQueue {
    Q q;
    bool push(const Msg& m) override { return q.try_push(m); }
    bool pop(Msg& m) override { return q.try_pop(m); }
};

void BM_Direct(benchmark::State& st) {
    auto q = std::make_unique<Q>();
    Msg v{};
    for (auto _ : st) {
        q->try_push(v);
        q->try_pop(v);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_Direct)->Name("abstraction/direct");

void BM_Concept(benchmark::State& st) {
    auto q = std::make_unique<Q>();
    Msg v{};
    for (auto _ : st) {
        generic_push_pop(*q, v);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_Concept)->Name("abstraction/template_concept");

void BM_Virtual(benchmark::State& st) {
    std::unique_ptr<IQueue> q = std::make_unique<VirtualSpsc>();
    IQueue* p = q.get();
    benchmark::DoNotOptimize(p);  // the optimiser can no longer see the dynamic type
    Msg v{};
    for (auto _ : st) {
        p->push(v);
        p->pop(v);
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_Virtual)->Name("abstraction/virtual");

}  // namespace

BENCHMARK_MAIN();
