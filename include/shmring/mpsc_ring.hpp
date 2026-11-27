#pragma once

// Bounded multi-producer / single-consumer ring (Dmitry Vyukov's bounded MPMC
// queue, with the consumer side simplified because there is only one).
//
// Every cell carries a sequence number that encodes whose turn it is:
//   seq == pos          cell is free for the producer that claims ticket `pos`
//   seq == pos + 1      cell holds data for the consumer at position `pos`
//   seq == pos + N      consumer freed it; free again for ticket pos + N
// Producers race on one counter with CAS; the winner owns the cell exclusively,
// writes it, then publishes with a release store of seq. The consumer never
// CASes -- it is the only reader -- so its index is a plain integer.
//
// Cross-process caveat (documented, not solved): a producer that dies between
// winning the CAS and publishing seq leaves a hole the consumer will wait on
// forever. Detecting that needs a per-producer heartbeat; see README.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "shmring/concepts.hpp"
#include "shmring/platform.hpp"

namespace shmring {

template <ShmPayload T, std::size_t N>
class MpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    using value_type = T;
    static constexpr std::size_t capacity = N;
    static constexpr bool multi_producer = true;

    MpscRing() noexcept {
        for (std::size_t i = 0; i < N; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    bool try_push(const T& v) noexcept {
        std::uint64_t pos = enq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & kMask];
            const std::uint64_t seq = c.seq.load(std::memory_order_acquire);
            const auto dif = std::int64_t(seq) - std::int64_t(pos);
            if (dif == 0) {
                // Free for this ticket: try to claim it.
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.data = v;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // CAS failure reloaded pos; retry.
            } else if (dif < 0) {
                return false;  // consumer has not freed this cell yet: full
            } else {
                pos = enq_.load(std::memory_order_relaxed);  // another producer got here first
            }
        }
    }

    bool try_pop(T& out) noexcept {
        Cell& c = cells_[deq_ & kMask];
        const std::uint64_t seq = c.seq.load(std::memory_order_acquire);
        if (seq != deq_ + 1) return false;  // empty (or the claiming producer has not published yet)
        out = c.data;
        c.seq.store(deq_ + N, std::memory_order_release);
        ++deq_;
        return true;
    }

    void push(const T& v) noexcept {
        while (!try_push(v)) cpu_relax();
    }

private:
    static constexpr std::uint64_t kMask = N - 1;

    // One cell per cache line, so two producers writing neighbouring tickets do
    // not false-share. Costs memory for small T; the trade is deliberate.
    struct alignas(kCacheLine) Cell {
        std::atomic<std::uint64_t> seq;
        T data;
    };

    alignas(kCacheLine) std::atomic<std::uint64_t> enq_{0};
    alignas(kCacheLine) std::uint64_t deq_{0};  // consumer-private
    alignas(kCacheLine) Cell cells_[N];
};

}  // namespace shmring
