#pragma once

// Single-writer seqlock for "latest value" data such as a top-of-book snapshot.
//
// A ring delivers every update; a seqlock delivers only the newest one and
// never makes the writer wait. That is the right contract for a reader that
// samples (a risk check wanting the current mid, a monitor) rather than one
// that must process every event.
//
//   writer: seq = s+1 (odd: "write in progress"); write words; seq = s+2
//   reader: s1 = seq; copy words; s2 = seq; valid iff s1 == s2 and s1 even
//
// Two details that are easy to get wrong:
//
//  1. The payload is stored as std::atomic<u64> words, not as a plain T. A
//     reader that copies a plain T while the writer is mid-write is a data race
//     -- undefined behaviour even though the result is thrown away. Relaxed
//     atomics cost nothing on x86 (plain MOVs) and make it defined, which is
//     also why ThreadSanitizer is clean on it.
//
//  2. No standalone fences. The data stores are release and the data loads are
//     acquire, which gives the two orderings needed: the writer's "seq is odd"
//     store cannot sink below a data store, and the reader's second seq load
//     cannot hoist above a data load. (TSan does not model
//     atomic_thread_fence, so the fence-based textbook version is also
//     un-checkable.)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shmring/concepts.hpp"
#include "shmring/platform.hpp"

namespace shmring {

template <ShmPayload T>
    requires(sizeof(T) % sizeof(std::uint64_t) == 0)
class Seqlock {
    static constexpr std::size_t W = sizeof(T) / sizeof(std::uint64_t);

public:
    using value_type = T;

    // Only one writer at a time, ever. Not checked.
    void store(const T& v) noexcept {
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::uint64_t w[W];
        std::memcpy(w, &v, sizeof(T));
        for (std::size_t i = 0; i < W; ++i) words_[i].store(w[i], std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    // One attempt. False if a write was in progress or happened during the copy.
    bool try_load(T& out) const noexcept {
        const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1) return false;
        std::uint64_t w[W];
        for (std::size_t i = 0; i < W; ++i) w[i] = words_[i].load(std::memory_order_acquire);
        const std::uint64_t s2 = seq_.load(std::memory_order_relaxed);
        if (s1 != s2) return false;
        std::memcpy(&out, w, sizeof(T));
        return true;
    }

    // Retry until consistent. `retries` (optional) counts failed attempts.
    T load(std::uint64_t* retries = nullptr) const noexcept {
        T out;
        while (!try_load(out)) {
            if (retries) ++*retries;
            cpu_relax();
        }
        return out;
    }

    std::uint64_t version() const noexcept { return seq_.load(std::memory_order_acquire) >> 1; }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> seq_{0};
    std::atomic<std::uint64_t> words_[W]{};
};

// A fixed table of seqlocks, one cache line (or more) each so that writes to
// symbol A never invalidate a reader of symbol B.
template <ShmPayload T, std::size_t N>
class SnapshotTable {
public:
    Seqlock<T>& operator[](std::size_t i) noexcept { return cells_[i]; }
    const Seqlock<T>& operator[](std::size_t i) const noexcept { return cells_[i]; }
    static constexpr std::size_t size() { return N; }

private:
    Seqlock<T> cells_[N];
};

}  // namespace shmring
