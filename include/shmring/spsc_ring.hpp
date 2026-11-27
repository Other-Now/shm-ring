#pragma once

// Bounded single-producer / single-consumer ring that lives in shared memory.
//
// Protocol (the whole thing):
//   producer: write slot[head]; head.store(head+1, release)
//   consumer: head.load(acquire); read slot[tail]; tail.store(tail+1, release)
// The release/acquire pair on `head` is what makes the slot write visible
// before the index; the pair on `tail` is what stops the producer reusing a
// slot the consumer is still reading. No CAS, no fences, no locks: on x86
// every one of these is a plain MOV.
//
// Indices are free-running u64 (never wrap in practice) and masked on use, so
// "full" is head - tail == N with no wasted slot.
//
// Layout is a template parameter so the false-sharing experiment is a 2x2 of
// (padding) x (cached opposite index) with otherwise identical code:
//
//   Packed        head + tail on one line; each side reads the other's index on
//                 every op -> the line is *truly* shared, padding cannot help
//   Padded        same algorithm, head and tail on separate lines
//   PackedCached  each side keeps a private copy of the other's index and only
//                 re-reads it when the ring looks full/empty -- but all four
//                 fields still share one line, so every tail store by the
//                 consumer invalidates the producer's head/cache line for no
//                 reason. This is the textbook *false* sharing case.
//   PaddedCached  the fix: producer line {head, tail_cache}, consumer line
//                 {tail, head_cache}. The lines only move when a cache misses.
//
// results/false_sharing.md has the measured 2x2.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "shmring/concepts.hpp"
#include "shmring/platform.hpp"

namespace shmring {

enum class Layout { Packed, Padded, PackedCached, PaddedCached };

inline constexpr bool cached(Layout l) { return l == Layout::PackedCached || l == Layout::PaddedCached; }

inline const char* layout_name(Layout l) {
    switch (l) {
        case Layout::Packed: return "packed";
        case Layout::Padded: return "padded";
        case Layout::PackedCached: return "packed+cached";
        case Layout::PaddedCached: return "padded+cached";
    }
    return "?";
}

namespace detail {

template <Layout L>
struct SpscIndices;

template <>
struct SpscIndices<Layout::Packed> {
    alignas(kCacheLine) std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};  // same line as head: this is the bug being demonstrated
    std::uint64_t head_cache{0}, tail_cache{0};  // used only by PackedCached
};

template <>
struct SpscIndices<Layout::PackedCached> : SpscIndices<Layout::Packed> {};

template <>
struct SpscIndices<Layout::Padded> {
    alignas(kCacheLine) std::atomic<std::uint64_t> head{0};
    std::uint64_t tail_cache{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> tail{0};
    std::uint64_t head_cache{0};
};

// Producer line: what it writes (head) + its private view of tail.
// Consumer line: what it writes (tail) + its private view of head.
template <>
struct SpscIndices<Layout::PaddedCached> : SpscIndices<Layout::Padded> {};

}  // namespace detail

template <ShmPayload T, std::size_t N, Layout L = Layout::PaddedCached>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "a lock-based atomic would put a process-local mutex in shared memory");

public:
    using value_type = T;
    static constexpr std::size_t capacity = N;
    static constexpr Layout layout = L;
    static constexpr bool multi_producer = false;

    bool try_push(const T& v) noexcept {
        const std::uint64_t h = idx_.head.load(std::memory_order_relaxed);  // we are the only writer
        if constexpr (cached(L)) {
            if (h - idx_.tail_cache == N) {
                idx_.tail_cache = idx_.tail.load(std::memory_order_acquire);
                if (h - idx_.tail_cache == N) return false;
            }
        } else {
            if (h - idx_.tail.load(std::memory_order_acquire) == N) return false;
        }
        slots_[h & kMask] = v;
        idx_.head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::uint64_t t = idx_.tail.load(std::memory_order_relaxed);  // we are the only writer
        if constexpr (cached(L)) {
            if (t == idx_.head_cache) {
                idx_.head_cache = idx_.head.load(std::memory_order_acquire);
                if (t == idx_.head_cache) return false;
            }
        } else {
            if (t == idx_.head.load(std::memory_order_acquire)) return false;
        }
        out = slots_[t & kMask];
        idx_.tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // Spin until there is room. Only for callers that are allowed to block.
    void push(const T& v) noexcept {
        while (!try_push(v)) cpu_relax();
    }

    // Approximate (racy by nature when called from a third party).
    std::size_t size() const noexcept {
        return std::size_t(idx_.head.load(std::memory_order_acquire) - idx_.tail.load(std::memory_order_acquire));
    }
    bool empty() const noexcept { return size() == 0; }

private:
    static constexpr std::uint64_t kMask = N - 1;
    detail::SpscIndices<L> idx_;
    alignas(kCacheLine) T slots_[N];
};

}  // namespace shmring
