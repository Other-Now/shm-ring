// Producer -> consumer throughput across two processes, for the false-sharing
// experiment and the MPSC ring.
//
//   ring_throughput --layout packed|padded|packed-cached|cached [--msgs N] [--cpus A,B]
//   ring_throughput --mpsc P [--msgs N]          P producer processes, 1 consumer
//
// Run it under perf stat to see the coherence traffic:
//   perf stat -e mem_load_l3_hit_retired.xsnp_hitm,... ./ring_throughput --layout packed
// perf stat follows forked children by default, so both processes are counted.
//
// Every message is a sequence number; the consumer checks order (SPSC: global
// FIFO; MPSC: FIFO per producer) and that nothing is lost or duplicated, so
// the benchmark doubles as a stress test.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "shmring/shmring.hpp"

using namespace shmring;

namespace {

constexpr std::size_t kCap = 1024;

struct Item {
    std::uint64_t v;  // (producer << 48) | seq
};

template <Queue Q>
int run(const std::string& label, std::size_t msgs, int producers, int cpu_a, int cpu_b) {
    const std::string name = "shmring_tp_" + std::to_string(::getpid());
    auto ch = Channel<Q>::create(name);

    std::vector<pid_t> kids;
    for (int p = 0; p < producers; ++p) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            pin_to_cpu(producers == 1 ? cpu_b : -1);
            auto q = Channel<Q>::attach(name);
            const std::uint64_t tag = std::uint64_t(p) << 48;
            for (std::uint64_t i = 0; i < msgs; ++i) q->push(Item{tag | i});
            std::_Exit(0);
        }
        kids.push_back(pid);
    }

    pin_to_cpu(cpu_a);
    std::vector<std::uint64_t> next(std::size_t(producers), 0);
    const std::size_t total = msgs * std::size_t(producers);
    std::size_t bad = 0;
    const std::uint64_t t0 = now_ns();
    for (std::size_t got = 0; got < total;) {
        Item it;
        if (!ch->try_pop(it)) {
            cpu_relax();
            continue;
        }
        const std::size_t p = std::size_t(it.v >> 48);
        const std::uint64_t s = it.v & ((1ull << 48) - 1);
        if (p >= next.size() || s != next[p]) ++bad;
        else ++next[p];
        ++got;
    }
    const std::uint64_t t1 = now_ns();
    for (pid_t k : kids) ::waitpid(k, nullptr, 0);

    const double secs = double(t1 - t0) / 1e9;
    std::fprintf(stderr, "%-14s producers %d  %zu msgs  %.1f M msg/s  %.2f ns/msg  order errors %zu\n",
                 label.c_str(), producers, total, double(total) / secs / 1e6, double(t1 - t0) / double(total), bad);
    std::printf("%s,%d,%zu,%.2f,%.3f,%zu\n", label.c_str(), producers, total, double(total) / secs / 1e6,
                double(t1 - t0) / double(total), bad);
    return bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string layout = "cached";
    int mpsc = 0, cpu_a = 1, cpu_b = 3;
    std::size_t msgs = 50'000'000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--layout") layout = next();
        else if (a == "--mpsc") mpsc = std::stoi(next());
        else if (a == "--msgs") msgs = std::stoull(next());
        else if (a == "--cpus") {
            const std::string v = next();
            std::sscanf(v.c_str(), "%d,%d", &cpu_a, &cpu_b);
        } else {
            std::fprintf(stderr, "usage: %s --layout packed|padded|packed-cached|cached | --mpsc P  [--msgs N] [--cpus A,B]\n",
                         argv[0]);
            return 2;
        }
    }
    if (mpsc > 0) return run<MpscRing<Item, kCap>>("mpsc", msgs, mpsc, cpu_a, cpu_b);
    if (layout == "packed") return run<SpscRing<Item, kCap, Layout::Packed>>("spsc-packed", msgs, 1, cpu_a, cpu_b);
    if (layout == "packed-cached")
        return run<SpscRing<Item, kCap, Layout::PackedCached>>("spsc-packed-cached", msgs, 1, cpu_a, cpu_b);
    if (layout == "padded") return run<SpscRing<Item, kCap, Layout::Padded>>("spsc-padded", msgs, 1, cpu_a, cpu_b);
    return run<SpscRing<Item, kCap, Layout::PaddedCached>>("spsc-cached", msgs, 1, cpu_a, cpu_b);
}
