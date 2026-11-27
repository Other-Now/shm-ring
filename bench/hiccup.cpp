// Platform jitter probe: how often does this core stop running our code?
//
//   hiccup [--cpu C] [--secs S] [--threshold-ns N]
//
// Spins on one pinned core reading the TSC back to back. Any gap larger than
// the threshold is time the core spent somewhere else (interrupt, scheduler,
// or -- under WSL2/Hyper-V -- the host descheduling the whole vCPU). No ring
// or IPC is involved, so whatever tail this shows is a floor that no IPC
// mechanism on this machine can beat. Used to attribute the cross-process
// tail in results/t2t_split.md.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "shmring/platform.hpp"

using namespace shmring;

int main(int argc, char** argv) {
    int cpu = 3;
    double secs = 10;
    std::uint64_t thr_ns = 1000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
        if (a == "--cpu") cpu = std::stoi(next());
        else if (a == "--secs") secs = std::stod(next());
        else if (a == "--threshold-ns") thr_ns = std::stoull(next());
    }
    pin_to_cpu(cpu);
    const double tpn = tsc_per_ns();
    const std::uint64_t thr = std::uint64_t(double(thr_ns) * tpn);
    const std::uint64_t end = rdtsc() + std::uint64_t(secs * 1e9 * tpn);
    std::vector<std::uint64_t> gaps;
    gaps.reserve(1 << 20);
    std::uint64_t prev = rdtsc(), lost = 0;
    while (prev < end) {
        const std::uint64_t now = rdtsc();
        if (now - prev > thr) {
            lost += now - prev;
            if (gaps.size() < gaps.capacity()) gaps.push_back(now - prev);
        }
        prev = now;
    }
    std::sort(gaps.begin(), gaps.end());
    auto ns = [&](std::uint64_t t) { return double(t) / tpn; };
    auto pct = [&](double p) { return gaps.empty() ? 0.0 : ns(gaps[std::min(gaps.size() - 1, std::size_t(p * double(gaps.size())))]); };
    std::fprintf(stderr,
                 "cpu %d, %.0f s: %zu gaps > %llu ns (%.1f/s)  gap p50 %.1f us  p99 %.1f us  max %.1f us  "
                 "time lost %.3f%%\n",
                 cpu, secs, gaps.size(), (unsigned long long)thr_ns, double(gaps.size()) / secs, pct(0.5) / 1e3,
                 pct(0.99) / 1e3, gaps.empty() ? 0.0 : ns(gaps.back()) / 1e3, 100.0 * ns(lost) / (secs * 1e9));
    std::printf("%d,%.0f,%zu,%.1f,%.1f,%.1f,%.4f\n", cpu, secs, gaps.size(), pct(0.5) / 1e3, pct(0.99) / 1e3,
                gaps.empty() ? 0.0 : ns(gaps.back()) / 1e3, 100.0 * ns(lost) / (secs * 1e9));
    return 0;
}
