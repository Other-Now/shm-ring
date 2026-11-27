#pragma once

// The few machine-level primitives everything else uses: the TSC, a spin hint,
// core pinning and a TSC->ns calibration. x86-64 Linux is the target; the
// fallbacks exist only so the headers compile elsewhere.

#include <cstddef>
#include <cstdint>
#include <ctime>

#include <sched.h>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace shmring {

// Destructive-interference size, written out instead of using
// std::hardware_destructive_interference_size: GCC warns that value can change
// between compiler flags, and this layout is shared between *processes* that
// must agree on it.
inline constexpr std::size_t kCacheLine = 64;

inline std::uint64_t now_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::uint64_t(ts.tv_sec) * 1'000'000'000ull + std::uint64_t(ts.tv_nsec);
}

// Plain rdtsc, not rdtscp. rdtscp waits for earlier instructions to retire,
// which in tick2trade cost tens of percent of the path it was timing. For
// "when did this message arrive / leave" stamps the non-serialising read is
// the right tool; the lfence variant below is for the round-trip harness,
// where one fence per ~200 ns round trip is noise.
inline std::uint64_t rdtsc() noexcept {
#if defined(__x86_64__)
    return __rdtsc();
#else
    return now_ns();
#endif
}

inline std::uint64_t rdtsc_fenced() noexcept {
#if defined(__x86_64__)
    _mm_lfence();
    return __rdtsc();
#else
    return now_ns();
#endif
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__)
    _mm_pause();
#endif
}

// Returns false if the CPU does not exist / is not allowed. cpu < 0 = do nothing.
inline bool pin_to_cpu(int cpu) noexcept {
    if (cpu < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
}

// TSC ticks per nanosecond, measured against CLOCK_MONOTONIC over `ms`.
// Relies on an invariant TSC (constant_tsc + nonstop_tsc), which every x86
// server and laptop of the last decade has.
inline double tsc_per_ns(unsigned ms = 200) noexcept {
    const std::uint64_t n0 = now_ns(), t0 = rdtsc_fenced();
    while (now_ns() - n0 < std::uint64_t(ms) * 1'000'000ull) {
    }
    const std::uint64_t n1 = now_ns(), t1 = rdtsc_fenced();
    return double(t1 - t0) / double(n1 - n0);
}

}  // namespace shmring
