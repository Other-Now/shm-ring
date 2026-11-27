// Unit + stress tests. No framework: CHECK() counts failures, main returns
// non-zero if any failed. Threaded tests are what ThreadSanitizer sees; the
// fork()-based tests exercise the real cross-process path (TSan cannot follow
// a race across processes, which is why both kinds exist).

#define SHMRING_COUNT_ALLOCS
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "shmring/shmring.hpp"

using namespace shmring;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_fail;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
        }                                                                       \
    } while (0)

#if defined(__SANITIZE_THREAD__)
constexpr std::uint64_t kStress = 200'000;  // TSan is ~10x slower
#else
constexpr std::uint64_t kStress = 5'000'000;
#endif

static std::string uniq(const char* base) { return std::string(base) + "_" + std::to_string(::getpid()); }

// ---------------------------------------------------------------- concepts

struct Pod {
    std::uint64_t a, b;
};
struct NotPod {
    std::string s;
};
static_assert(ShmPayload<Pod>);
static_assert(!ShmPayload<NotPod>);
static_assert(Queue<SpscRing<Pod, 8>>);
static_assert(Queue<MpscRing<Pod, 8>>);
static_assert(std::is_same_v<Ring<Pod, 8, Producers::One>, SpscRing<Pod, 8>>);
static_assert(std::is_same_v<Ring<Pod, 8, Producers::Many>, MpscRing<Pod, 8>>);
// Layout really changes the layout:
static_assert(sizeof(SpscRing<std::uint64_t, 8, Layout::Packed>) <
              sizeof(SpscRing<std::uint64_t, 8, Layout::PaddedCached>));

// Generic code written against the concept, instantiated per queue type.
template <Queue Q>
static std::uint64_t fill_then_drain(Q& q) {
    std::uint64_t pushed = 0, sum = 0;
    while (q.try_push(typename Q::value_type{pushed, 0})) ++pushed;
    typename Q::value_type v{};
    while (q.try_pop(v)) sum += v.a;
    return pushed * 1000 + (sum == pushed * (pushed - 1) / 2 ? 1 : 0);
}

template <Layout L>
static void spsc_single_thread() {
    auto q = std::make_unique<SpscRing<Pod, 8, L>>();
    CHECK(fill_then_drain(*q) == 8 * 1000 + 1);  // exactly N fit, FIFO
    // Wrap-around many times.
    Pod v{};
    for (std::uint64_t i = 0; i < 1000; ++i) {
        CHECK(q->try_push(Pod{i, ~i}));
        CHECK(q->try_pop(v));
        CHECK(v.a == i && v.b == ~i);
    }
    CHECK(!q->try_pop(v));
    CHECK(q->empty());
}

static void mpsc_single_thread() {
    auto q = std::make_unique<MpscRing<Pod, 8>>();
    CHECK(fill_then_drain(*q) == 8 * 1000 + 1);
    Pod v{};
    for (std::uint64_t i = 0; i < 1000; ++i) {
        CHECK(q->try_push(Pod{i, 0}));
        CHECK(q->try_pop(v) && v.a == i);
    }
    CHECK(!q->try_pop(v));
}

// --------------------------------------------------------- threaded stress

template <Layout L>
static void spsc_threads() {
    auto q = std::make_unique<SpscRing<Pod, 64, L>>();
    std::thread prod([&] {
        for (std::uint64_t i = 0; i < kStress; ++i) q->push(Pod{i, i * 7});
    });
    std::uint64_t expect = 0, bad = 0;
    Pod v{};
    while (expect < kStress) {
        if (!q->try_pop(v)) continue;
        if (v.a != expect || v.b != expect * 7) ++bad;
        ++expect;
    }
    prod.join();
    CHECK(bad == 0);
}

static void mpsc_threads() {
    constexpr int P = 3;
    auto q = std::make_unique<MpscRing<Pod, 64>>();
    std::vector<std::thread> ts;
    for (int p = 0; p < P; ++p)
        ts.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < kStress / P; ++i) q->push(Pod{std::uint64_t(p), i});
        });
    std::vector<std::uint64_t> next(P, 0);
    std::uint64_t got = 0, bad = 0;
    Pod v{};
    while (got < (kStress / P) * P) {
        if (!q->try_pop(v)) continue;
        if (v.a >= P || v.b != next[v.a]) ++bad;  // FIFO per producer, no loss, no dup
        else ++next[v.a];
        ++got;
    }
    for (auto& t : ts) t.join();
    CHECK(bad == 0);
    for (int p = 0; p < P; ++p) CHECK(next[p] == kStress / P);
}

// Writer stores {k, k, k, k}; a torn read would show unequal words.
struct Quad {
    std::uint64_t w[4];
};

static void seqlock_threads() {
    auto sl = std::make_unique<Seqlock<Quad>>();
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> torn{0}, reads{0}, retries{0}, regress{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r)
        readers.emplace_back([&] {
            std::uint64_t last = 0, n = 0, rt = 0;
            while (!done.load(std::memory_order_acquire)) {
                const Quad q = sl->load(&rt);
                if (q.w[0] != q.w[1] || q.w[1] != q.w[2] || q.w[2] != q.w[3]) torn.fetch_add(1);
                if (q.w[0] < last) regress.fetch_add(1);  // must never go backwards
                last = q.w[0];
                ++n;
            }
            reads.fetch_add(n);
            retries.fetch_add(rt);
        });
    for (std::uint64_t k = 1; k <= kStress; ++k) sl->store(Quad{{k, k, k, k}});
    done.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
    CHECK(torn.load() == 0);
    CHECK(regress.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(sl->version() == kStress);
    std::fprintf(stderr, "  seqlock: %llu reads, %llu retries, 0 torn\n", (unsigned long long)reads.load(),
                 (unsigned long long)retries.load());
}

// ------------------------------------------------------------ cross-process

static void spsc_cross_process() {
    using Q = SpscRing<Pod, 256>;
    const std::string name = uniq("shmring_test_spsc");
    auto ch = Channel<Q>::create(name);
    const pid_t kid = ::fork();
    if (kid == 0) {
        auto q = Channel<Q>::attach(name);  // separate mapping, different address
        for (std::uint64_t i = 0; i < kStress; ++i) q->push(Pod{i, ~i});
        std::_Exit(0);
    }
    std::uint64_t expect = 0, bad = 0;
    Pod v{};
    while (expect < kStress) {
        if (!ch->try_pop(v)) { cpu_relax(); continue; }
        if (v.a != expect || v.b != ~expect) ++bad;
        ++expect;
    }
    int st = 0;
    ::waitpid(kid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(bad == 0);
}

static void mpsc_cross_process() {
    constexpr int P = 3;
    using Q = MpscRing<Pod, 256>;
    const std::string name = uniq("shmring_test_mpsc");
    auto ch = Channel<Q>::create(name);
    std::vector<pid_t> kids;
    const std::uint64_t per = kStress / P;
    for (int p = 0; p < P; ++p) {
        const pid_t kid = ::fork();
        if (kid == 0) {
            auto q = Channel<Q>::attach(name);
            for (std::uint64_t i = 0; i < per; ++i) q->push(Pod{std::uint64_t(p), i});
            std::_Exit(0);
        }
        kids.push_back(kid);
    }
    std::vector<std::uint64_t> next(P, 0);
    std::uint64_t got = 0, bad = 0;
    Pod v{};
    while (got < per * P) {
        if (!ch->try_pop(v)) { cpu_relax(); continue; }
        if (v.a >= P || v.b != next[v.a]) ++bad;
        else ++next[v.a];
        ++got;
    }
    for (pid_t k : kids) ::waitpid(k, nullptr, 0);
    CHECK(bad == 0);
}

static void attach_validation() {
    const std::string name = uniq("shmring_test_type");
    auto a = Channel<SpscRing<Pod, 64>>::create(name);
    bool threw = false;
    try {
        auto b = Channel<SpscRing<Pod, 128>>::attach(name, std::chrono::milliseconds(200));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);  // wrong type must not attach
    threw = false;
    try {
        auto c = Channel<SpscRing<Pod, 64>>::attach(uniq("shmring_does_not_exist"), std::chrono::milliseconds(50));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    auto ok = Channel<SpscRing<Pod, 64>>::attach(name);
    CHECK(ok->try_push(Pod{1, 2}));
    Pod v{};
    CHECK(a->try_pop(v) && v.a == 1 && v.b == 2);  // same memory through both mappings
    CHECK(a.segment().page_size_kb() == 4 || a.segment().page_size_kb() == 2048);
}

static void huge_pages_fallback() {
    // Try never fails: it either gets 2 MiB pages or falls back to tmpfs, and
    // smaps must agree with what it claims.
    auto s = Shared<SpscRing<Pod, 64>>::create(uniq("shmring_test_huge"), HugePages::Try);
    const std::size_t kb = s.segment().page_size_kb();
    CHECK(kb == (s.segment().huge() ? 2048u : 4u));
    std::fprintf(stderr, "  huge pages: %s (KernelPageSize %zu kB)\n", s.segment().huge() ? "yes" : "no (fallback)",
                 kb);
}

// ------------------------------------------------------------- object pool

struct Tracked {
    static inline int live = 0;
    std::uint64_t v;
    explicit Tracked(std::uint64_t x) : v(x) { ++live; }
    ~Tracked() { --live; }
};

static void object_pool() {
    auto pool = std::make_unique<ObjectPool<Tracked, 16>>();
    std::vector<Tracked*> got;
    got.reserve(32);
    const std::uint64_t a0 = alloc_count();
    for (int i = 0; i < 16; ++i) got.push_back(pool->acquire(std::uint64_t(i)));
    CHECK(pool->acquire(99) == nullptr);  // exhausted -> nullptr, not a heap fallback
    CHECK(Tracked::live == 16);
    for (int i = 0; i < 16; ++i) CHECK(got[i]->v == std::uint64_t(i) && pool->owns(got[i]));
    for (auto* p : got) pool->release(p);
    CHECK(Tracked::live == 0);
    CHECK(pool->available() == 16);
    // LIFO: the last one released is the next one handed out.
    Tracked* x = pool->acquire(1);
    CHECK(x == got.back());
    pool->release(x);
    // A million acquire/release cycles: zero heap allocations.
    for (int i = 0; i < 1'000'000; ++i) {
        auto h = pool->make(std::uint64_t(i));
        CHECK(h->v == std::uint64_t(i));
    }
    CHECK(alloc_count() == a0);
    CHECK(Tracked::live == 0);
}

// Steady-state push/pop allocates nothing either.
static void ring_no_alloc() {
    auto q = std::make_unique<SpscRing<Pod, 64>>();
    const std::uint64_t a0 = alloc_count();
    Pod v{};
    for (std::uint64_t i = 0; i < 1'000'000; ++i) {
        q->try_push(Pod{i, i});
        q->try_pop(v);
    }
    CHECK(alloc_count() == a0);
}

int main() {
    std::fprintf(stderr, "single-thread\n");
    spsc_single_thread<Layout::Packed>();
    spsc_single_thread<Layout::Padded>();
    spsc_single_thread<Layout::PackedCached>();
    spsc_single_thread<Layout::PaddedCached>();
    mpsc_single_thread();
    std::fprintf(stderr, "threads (%llu msgs)\n", (unsigned long long)kStress);
    spsc_threads<Layout::Packed>();
    spsc_threads<Layout::PaddedCached>();
    mpsc_threads();
    seqlock_threads();
    std::fprintf(stderr, "cross-process\n");
    spsc_cross_process();
    mpsc_cross_process();
    attach_validation();
    huge_pages_fallback();
    std::fprintf(stderr, "pool / allocations\n");
    object_pool();
    ring_no_alloc();
    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
