#pragma once

// The book, shaped by what the *strategy* asks of it rather than by what a
// general order book ought to support.
//
// The signal on the hot path needs exactly one thing: best bid/ask and the size
// resting there, read on every single event. It never walks depth. So the design
// target is "top-of-book read is a load, not a search", and everything else is
// allowed to be merely adequate.
//
// Orders live in an open-addressed table keyed by the 64-bit reference, with the
// entry carrying its symbol slot and price so cancel/execute/delete never touch
// the symbol map. Price levels are a sorted flat vector per side: insertion is
// memmove, which sounds bad and is not -- real depth near the touch is shallow,
// the vectors are cache-resident, and memmove of a few hundred bytes beats
// pointer-chasing a tree. This is measured in the README, not assumed.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace t2t {

struct Level {
    std::uint32_t price;
    std::uint32_t shares;
};

// One side of one symbol. Bids sort descending, asks ascending, so index 0 is
// always the touch and the strategy's read is levels_[0].
template <bool Descending>
class Side {
public:
    void add(std::uint32_t price, std::uint32_t shares) {
        const auto it = find_slot(price);
        if (it != levels_.end() && it->price == price) { it->shares += shares; return; }
        levels_.insert(it, Level{price, shares});
    }

    void remove(std::uint32_t price, std::uint32_t shares) {
        const auto it = find_slot(price);
        if (it == levels_.end() || it->price != price) return;
        if (it->shares <= shares) levels_.erase(it);   // level emptied -> gone
        else it->shares -= shares;
    }

    bool empty() const { return levels_.empty(); }
    // shm-ring patch: lets the feed handler pre-size levels so steady state
    // does not allocate.
    void reserve(std::size_t n) { levels_.reserve(n); }
    const Level& top() const { return levels_.front(); }
    std::size_t depth() const { return levels_.size(); }

private:
    std::vector<Level>::iterator find_slot(std::uint32_t price) {
        if constexpr (Descending) {
            return std::lower_bound(levels_.begin(), levels_.end(), price,
                                    [](const Level& l, std::uint32_t p) { return l.price > p; });
        } else {
            return std::lower_bound(levels_.begin(), levels_.end(), price,
                                    [](const Level& l, std::uint32_t p) { return l.price < p; });
        }
    }
    std::vector<Level> levels_;
};

struct Book {
    Side<true>  bid;
    Side<false> ask;
    void reserve(std::size_t n) { bid.reserve(n); ask.reserve(n); }  // shm-ring patch
};

// Open-addressed order table. std::unordered_map costs a pointer chase per
// lookup and an allocation per insert; on a feed that adds and deletes millions
// of orders, both land directly on the hot path.
class OrderTable {
public:
    struct Entry {
        std::uint64_t ref;      // 0 == empty slot
        std::uint32_t sym_slot;
        std::uint32_t price;
        std::uint32_t shares;
        bool          buy;
    };

    explicit OrderTable(std::size_t capacity_pow2 = (1u << 22)) : mask_(capacity_pow2 - 1) {
        slots_.resize(capacity_pow2);
    }

    inline Entry* find(std::uint64_t ref) noexcept {
        std::size_t i = hash(ref) & mask_;
        for (;;) {
            Entry& e = slots_[i];
            if (e.ref == ref) return &e;
            if (e.ref == 0) return nullptr;
            i = (i + 1) & mask_;
        }
    }

    inline void insert(const Entry& v) noexcept {
        std::size_t i = hash(v.ref) & mask_;
        for (;;) {
            Entry& e = slots_[i];
            if (e.ref == 0 || e.ref == v.ref) { e = v; ++size_; return; }
            i = (i + 1) & mask_;
        }
    }

    // Backward-shift deletion: keeps the probe sequence intact without
    // tombstones, so a long-running feed does not degrade toward linear scans.
    inline void erase(Entry* e) noexcept {
        if (!e) return;
        std::size_t i = static_cast<std::size_t>(e - slots_.data());
        slots_[i].ref = 0;
        --size_;
        std::size_t j = (i + 1) & mask_;
        while (slots_[j].ref != 0) {
            const std::size_t home = hash(slots_[j].ref) & mask_;
            // Is `home` cyclically within (i, j]? If not, the entry may move up.
            const bool in_between = (i <= j) ? (home > i && home <= j)
                                             : (home > i || home <= j);
            if (!in_between) {
                slots_[i] = slots_[j];
                slots_[j].ref = 0;
                i = j;
            }
            j = (j + 1) & mask_;
        }
    }

    std::size_t size() const { return size_; }

private:
    // splitmix64 finaliser: ITCH references are dense and sequential, which is
    // the worst case for identity hashing into a power-of-two table.
    static inline std::uint64_t hash(std::uint64_t x) noexcept {
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27; x *= 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

    std::vector<Entry> slots_;
    std::size_t mask_;
    std::size_t size_{0};
};

}  // namespace t2t
