// Negative control for the ThreadSanitizer run.
//
// "TSan is clean" only means something if TSan would have caught the bug this
// code is most likely to have. This is the SPSC ring with the one ordering that
// matters weakened: the producer publishes `head` with a relaxed store instead
// of release. On x86 that compiles to the very same MOV -- the hardware would
// hide the bug -- but the C++ memory model says the consumer may read the slot
// before the producer's write to it, and TSan must report a data race on the
// slot. CI runs this expecting a non-zero exit.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

struct BrokenRing {
    std::atomic<std::uint64_t> head{0}, tail{0};
    std::uint64_t slots[64];

    bool push(std::uint64_t v) {
        const auto h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) == 64) return false;
        slots[h & 63] = v;
        head.store(h + 1, std::memory_order_relaxed);  // BUG: must be release
        return true;
    }
    bool pop(std::uint64_t& v) {
        const auto t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_relaxed)) return false;  // BUG: must be acquire
        v = slots[t & 63];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
};

int main() {
    static BrokenRing q;
    constexpr std::uint64_t n = 100000;
    std::thread p([] {
        for (std::uint64_t i = 0; i < n; ++i)
            while (!q.push(i)) {
            }
    });
    std::uint64_t got = 0, v = 0, sum = 0;
    while (got < n)
        if (q.pop(v)) { sum += v; ++got; }
    p.join();
    std::printf("sum %llu\n", (unsigned long long)sum);
    return 0;
}
