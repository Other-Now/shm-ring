// tick2trade split across a process boundary.
//
//   t2t_split feed     <itch-file> [opts]   feed handler process (creates the shm)
//   t2t_split strategy [opts]               strategy + risk + OUCH process (attaches)
//   t2t_split run      <itch-file> [opts]   fork both of the above (convenience)
//   t2t_split inproc   <itch-file> [opts]   same code, one process, no ring (baseline)
//
// opts:
//   --gap-ns N     pace book events N ns apart (0 = as fast as possible).
//                  Latency is only meaningful when paced: unpaced, a fast
//                  producer fills the ring and you measure queueing.
//   --max N        stop after N ITCH messages
//   --warmup N     ignore the first N messages for latency and allocation counts
//   --cpu-feed C / --cpu-strat C   pin (default 1 and 3: different physical cores here)
//   --no-reserve   do not pre-size book levels (tick2trade's original behaviour)
//   --no-snapshot  risk reference price from the update, not the seqlock table
//
// Tick-to-order = TSC after the OUCH encode (strategy) - TSC before the ITCH
// decode (feed). Both processes read the same invariant TSC, so the difference
// is meaningful across cores; only accepted orders produce a sample.

#define SHMRING_COUNT_ALLOCS
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shmring/shmring.hpp"
#include "t2t_core.hpp"

using namespace shmring;
using namespace t2ts;

namespace {

const char* kRingName = "shmring_t2t_tob";
const char* kSnapName = "shmring_t2t_snap";
const char* kCtlName = "shmring_t2t_ctl";

struct Control {
    std::atomic<std::uint32_t> strategy_ready{0};
};

struct Opts {
    std::string file;
    std::uint64_t gap_ns = 0, max_msgs = ~0ull, warmup = 200'000;
    int cpu_feed = 1, cpu_strat = 3;
    bool reserve = true, snapshot = true;
};

constexpr std::uint32_t kFlagWarmup = 1;

struct MappedFile {
    const unsigned char* p = nullptr;
    std::size_t n = 0;
    explicit MappedFile(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { std::perror(path.c_str()); std::exit(1); }
        struct stat st {};
        ::fstat(fd, &st);
        n = std::size_t(st.st_size);
        void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
        p = static_cast<const unsigned char*>(m);
    }
    ~MappedFile() { ::munmap(const_cast<unsigned char*>(p), n); }
};

// Walk BinaryFILE framing (2-byte big-endian length, then the message), call
// f(msg, len, index) for each, pacing book events `gap` TSC ticks apart.
template <class F>
void replay(const MappedFile& mf, const Opts& o, std::uint64_t gap_ticks, F&& f) {
    std::size_t off = 0;
    std::uint64_t idx = 0, next_slot = 0;
    while (off + 2 <= mf.n && idx < o.max_msgs) {
        const std::uint16_t len = t2t::itch::be16(mf.p + off);
        if (off + 2 + len > mf.n) break;
        if (gap_ticks) {
            std::uint64_t now;
            while ((now = rdtsc()) < next_slot) cpu_relax();
            if (f(mf.p + off + 2, len, idx)) next_slot = now + gap_ticks;
        } else {
            f(mf.p + off + 2, len, idx);
        }
        off += 2 + std::size_t(len);
        ++idx;
    }
}

// TSC-tick samples in pre-reserved vectors (no allocation while recording).
struct Samples {
    std::vector<std::uint32_t> v;
    explicit Samples(std::size_t n) { v.reserve(n); }
    void add(std::uint64_t ticks) {
        if (v.size() < v.capacity()) v.push_back(std::uint32_t(std::min<std::uint64_t>(ticks, 0xffffffffu)));
    }
    double pct(double p, double tpn) const {
        if (v.empty()) return 0;
        return double(v[std::min(v.size() - 1, std::size_t(p * double(v.size())))]) / tpn;
    }
    void sort() { std::sort(v.begin(), v.end()); }
};

struct Report {
    Samples t2o{1'000'000};      // tick-to-order, accepted orders only
    Samples feed{1'000'000};     // of those: decode+book+snapshot (t_push - t_rx)
    Samples hop{1'000'000};      // of those: ring transit (t_pop - t_push); ~0 in-process
    Samples strat{1'000'000};    // of those: signal+pool+risk+encode (t_out - t_pop)
    Samples transit{8'000'000};  // ring transit for *every* update, not only orders
    std::uint64_t allocs_at_steady = 0;
    void order(const TobUpdate& u, std::uint64_t t_pop, std::uint64_t t_out) {
        t2o.add(t_out - u.t_rx);
        feed.add(u.t_push - u.t_rx);
        hop.add(t_pop - u.t_push);
        strat.add(t_out - t_pop);
    }
};

void print_report(const char* mode, Report& r, const StratStats& s, double tpn, std::uint64_t feed_msgs,
                  std::uint64_t allocs_end) {
    for (Samples* x : {&r.t2o, &r.feed, &r.hop, &r.strat, &r.transit}) x->sort();
    double mean = 0;
    for (auto v : r.t2o.v) mean += double(v);
    mean = r.t2o.v.empty() ? 0 : mean / double(r.t2o.v.size()) / tpn;
    const double mx = r.t2o.v.empty() ? 0.0 : double(r.t2o.v.back()) / tpn;
    const std::uint64_t steady_allocs = allocs_end - r.allocs_at_steady;
    std::fprintf(stderr, "[%s] updates %llu  signals %llu  accepted %llu  rejected %llu  pool_empty %llu\n", mode,
                 (unsigned long long)s.updates, (unsigned long long)s.signals, (unsigned long long)s.accepted,
                 (unsigned long long)s.rejected, (unsigned long long)s.pool_empty);
    std::fprintf(stderr, "[%s] %-27s %9s %9s %9s\n", mode, "ns (TSC)", "p50", "p99", "p99.9");
    auto row = [&](const char* name, const Samples& x) {
        std::fprintf(stderr, "[%s] %-27s %9.0f %9.0f %9.0f   (n=%zu)\n", mode, name, x.pct(0.5, tpn),
                     x.pct(0.99, tpn), x.pct(0.999, tpn), x.v.size());
    };
    row("tick-to-order", r.t2o);
    row("  feed: decode+book+snap", r.feed);
    row("  ring hop", r.hop);
    row("  strategy+risk+encode", r.strat);
    row("ring transit, all updates", r.transit);
    std::fprintf(stderr, "[%s] OUCH checksum %016llx   steady-state operator-new calls (strategy side): %llu\n", mode,
                 (unsigned long long)s.checksum, (unsigned long long)steady_allocs);
    for (std::size_t i = 1; i < std::size_t(arg::Reason::kCount); ++i)
        if (s.reject_by_reason[i])
            std::fprintf(stderr, "[%s]   reject %-18s %llu\n", mode, arg::reason_name(arg::Reason(i)),
                         (unsigned long long)s.reject_by_reason[i]);
    // mode,msgs,updates,orders,p50,p99,p999,max,mean,checksum,allocs,
    // feed_p50,hop_p50,hop_p99,strat_p50,transit_p50,transit_p99,transit_p999
    std::printf("%s,%llu,%llu,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%016llx,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", mode,
                (unsigned long long)feed_msgs, (unsigned long long)s.updates, r.t2o.v.size(), r.t2o.pct(0.5, tpn),
                r.t2o.pct(0.99, tpn), r.t2o.pct(0.999, tpn), mx, mean, (unsigned long long)s.checksum,
                (unsigned long long)steady_allocs, r.feed.pct(0.5, tpn), r.hop.pct(0.5, tpn), r.hop.pct(0.99, tpn),
                r.strat.pct(0.5, tpn), r.transit.pct(0.5, tpn), r.transit.pct(0.99, tpn), r.transit.pct(0.999, tpn));
    std::fflush(stdout);
}

// ------------------------------------------------------------------ modes

int run_feed(const Opts& o) {
    pin_to_cpu(o.cpu_feed);
    const double tpn = tsc_per_ns();
    MappedFile mf(o.file);
    auto feed = std::make_unique<FeedCore>(o.reserve);
    auto ring = Channel<TobRing>::create(kRingName);
    auto snaps = Shared<TobTable>::create(kSnapName);
    auto ctl = Shared<Control>::create(kCtlName);

    while (!ctl->strategy_ready.load(std::memory_order_acquire)) cpu_relax();

    std::uint64_t allocs_steady = 0;
    TobUpdate u{};
    replay(mf, o, std::uint64_t(double(o.gap_ns) * tpn), [&](const unsigned char* m, std::uint16_t len,
                                                            std::uint64_t idx) {
        if (idx == o.warmup) {
            allocs_steady = alloc_count();
            if (std::getenv("SHMRING_ALLOC_TRACE")) alloc_trace_arm(true);
        }
        const std::uint64_t t = rdtsc();
        if (!feed->on_message(m, len, t, u)) return false;
        u.flags = idx < o.warmup ? kFlagWarmup : 0;
        (*snaps)[u.slot].store(Tob{u.bid_px, u.bid_sz, u.ask_px, u.ask_sz});  // snapshot first ...
        u.t_push = rdtsc();
        ring->push(u);                                                          // ... then the event
        return true;
    });
    alloc_trace_arm(false);
    const std::uint64_t allocs_end = alloc_count();
    TobUpdate end{};
    end.slot = kEndOfFeed;
    ring->push(end);
    std::fprintf(stderr, "[feed] messages %llu  book events %llu  unknown refs %llu  steady-state operator-new calls: %llu\n",
                 (unsigned long long)feed->messages(), (unsigned long long)feed->book_events(),
                 (unsigned long long)feed->unknown_refs(), (unsigned long long)(allocs_end - allocs_steady));
    // Keep the segments alive until the strategy has drained the ring.
    while (!ring->empty()) cpu_relax();
    ::usleep(100'000);
    return 0;
}

int run_strategy(const Opts& o) {
    pin_to_cpu(o.cpu_strat);
    const double tpn = tsc_per_ns();
    auto strat = std::make_unique<StrategyCore>(o.snapshot);
    auto rep = std::make_unique<Report>();
    auto ring = Channel<TobRing>::attach(kRingName, std::chrono::seconds(30));
    auto snaps = Shared<TobTable>::attach(kSnapName, std::chrono::seconds(30));
    auto ctl = Shared<Control>::attach(kCtlName, std::chrono::seconds(30));
    ctl->strategy_ready.store(1, std::memory_order_release);

    bool steady = false;
    TobUpdate u;
    for (;;) {
        if (!ring->try_pop(u)) { cpu_relax(); continue; }
        const std::uint64_t t_pop = rdtsc();
        if (u.slot == kEndOfFeed) break;
        const bool warm = u.flags & kFlagWarmup;
        if (!steady && !warm) {
            steady = true;
            rep->allocs_at_steady = alloc_count();
            if (std::getenv("SHMRING_ALLOC_TRACE")) alloc_trace_arm(true);
        }
        if (!warm) rep->transit.add(t_pop - u.t_push);
        std::uint64_t t_out = 0;
        if (strat->on_update(u, *snaps, t_out) && !warm) rep->order(u, t_pop, t_out);
    }
    alloc_trace_arm(false);
    if (!steady) rep->allocs_at_steady = alloc_count();
    print_report("xproc", *rep, strat->stats(), tpn, 0, alloc_count());
    return 0;
}

int run_inproc(const Opts& o) {
    pin_to_cpu(o.cpu_feed);
    const double tpn = tsc_per_ns();
    MappedFile mf(o.file);
    auto feed = std::make_unique<FeedCore>(o.reserve);
    auto strat = std::make_unique<StrategyCore>(o.snapshot);
    auto snaps = std::make_unique<TobTable>();
    auto rep = std::make_unique<Report>();
    bool steady = false;
    TobUpdate u{};
    replay(mf, o, std::uint64_t(double(o.gap_ns) * tpn), [&](const unsigned char* m, std::uint16_t len,
                                                            std::uint64_t idx) {
        if (idx == o.warmup) {
            steady = true;
            rep->allocs_at_steady = alloc_count();
            if (std::getenv("SHMRING_ALLOC_TRACE")) alloc_trace_arm(true);
        }
        const std::uint64_t t = rdtsc();
        if (!feed->on_message(m, len, t, u)) return false;
        (*snaps)[u.slot].store(Tob{u.bid_px, u.bid_sz, u.ask_px, u.ask_sz});
        u.t_push = rdtsc();
        const std::uint64_t t_pop = u.t_push;  // no ring: the "hop" is a function call
        std::uint64_t t_out = 0;
        if (strat->on_update(u, *snaps, t_out) && idx >= o.warmup) rep->order(u, t_pop, t_out);
        return true;
    });
    alloc_trace_arm(false);
    if (!steady) rep->allocs_at_steady = alloc_count();
    std::fprintf(stderr, "[inproc] messages %llu  book events %llu\n", (unsigned long long)feed->messages(),
                 (unsigned long long)feed->book_events());
    print_report("inproc", *rep, strat->stats(), tpn, feed->messages(), alloc_count());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s feed|strategy|run|inproc [itch-file] [--gap-ns N] [--max N] [--warmup N] "
                             "[--cpu-feed C] [--cpu-strat C] [--no-reserve]\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    Opts o;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
        if (a == "--gap-ns") o.gap_ns = std::stoull(next());
        else if (a == "--max") o.max_msgs = std::stoull(next());
        else if (a == "--warmup") o.warmup = std::stoull(next());
        else if (a == "--cpu-feed") o.cpu_feed = std::stoi(next());
        else if (a == "--cpu-strat") o.cpu_strat = std::stoi(next());
        else if (a == "--no-reserve") o.reserve = false;
        else if (a == "--no-snapshot") o.snapshot = false;
        else if (a.rfind("--", 0) != 0 && o.file.empty()) o.file = a;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    try {
        if (mode == "feed") return run_feed(o);
        if (mode == "strategy") return run_strategy(o);
        if (mode == "inproc") return run_inproc(o);
        if (mode == "run") {
            // A crashed earlier run can leave valid-looking segments behind; the
            // strategy must not attach to those before the feed recreates them.
            for (const char* n : {kRingName, kSnapName, kCtlName}) ::shm_unlink((std::string("/") + n).c_str());
            const pid_t kid = ::fork();
            if (kid == 0) std::_Exit(run_strategy(o));
            const int rc = run_feed(o);
            int st = 0;
            ::waitpid(kid, &st, 0);
            return rc != 0 ? rc : (WIFEXITED(st) ? WEXITSTATUS(st) : 1);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
