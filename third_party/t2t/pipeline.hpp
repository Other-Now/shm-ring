#pragma once

// The tick-to-trade path itself: decode -> book -> signal -> encode.
//
// Kept in one header so the benchmark and the tests drive the identical code,
// and so the stage boundaries are visible in one place. The stages are separate
// inlineable functions rather than one blob specifically so the harness can also
// run them individually for batch-amortised per-stage costs.

#include <cstdint>
#include <vector>

#include "t2t/book.hpp"
#include "t2t/itch.hpp"
#include "t2t/ouch.hpp"
#include "t2t/strategy.hpp"

namespace t2t {

// Symbol -> dense slot. Open-addressed for the same reason as OrderTable: this
// is read on every add and must not chase pointers.
class SymbolTable {
public:
    explicit SymbolTable(std::size_t cap_pow2 = (1u << 14)) : mask_(cap_pow2 - 1) {
        keys_.assign(cap_pow2, 0);
        slots_.assign(cap_pow2, 0);
    }

    inline std::uint32_t intern(std::uint64_t sym) noexcept {
        std::size_t i = mix(sym) & mask_;
        for (;;) {
            if (keys_[i] == sym) return slots_[i];
            if (keys_[i] == 0) {
                keys_[i] = sym;
                slots_[i] = next_++;
                return slots_[i];
            }
            i = (i + 1) & mask_;
        }
    }
    std::uint32_t count() const { return next_; }

private:
    static inline std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        return x;
    }
    std::vector<std::uint64_t> keys_;
    std::vector<std::uint32_t> slots_;
    std::size_t mask_;
    std::uint32_t next_{0};
};

struct Stats {
    std::uint64_t messages{0};
    std::uint64_t book_events{0};   // messages that actually moved a book
    std::uint64_t orders_sent{0};
    std::uint64_t unknown_refs{0};  // execute/cancel/delete for an unseen order
    std::uint64_t len_mismatch{0};  // declared length != spec length
};

class Pipeline {
public:
    explicit Pipeline(std::size_t max_symbols = 10000)
        : strat_(500, 3, 1, 100) {   // <=5c spread, 3:1 imbalance, 100-share clip
        books_.resize(max_symbols);
    }

    // One event, end to end. Returns true if an order was encoded into out_.
    // Marked always_inline: the whole point is to measure the path as the
    // compiler would actually build it, not to measure a call boundary.
    __attribute__((always_inline)) inline bool on_message(const unsigned char* m,
                                                          std::uint16_t len) noexcept {
        ++st_.messages;
        const char type = static_cast<char>(m[0]);
        const int expect = itch::wire_len(type);
        if (expect < 0) return false;                 // not a book-moving message
        if (static_cast<int>(len) != expect) { ++st_.len_mismatch; return false; }

        Book* b = apply(type, m);                     // decode + book update
        if (!b) return false;
        ++st_.book_events;

        const Signal s = strat_.on_book(*b);          // signal
        if (!s.send) return false;

        const ouch::EnterOrder o{++token_, last_sym_, s.shares, s.price, s.buy};
        out_len_ = ouch::encode(out_, o);             // order encode
        ++st_.orders_sent;
        return true;
    }

    // ---- stages, exposed for batch-amortised measurement -------------------

    __attribute__((always_inline)) inline Book* apply(char type,
                                                      const unsigned char* m) noexcept {
        switch (type) {
            case 'A': case 'F': {
                const itch::AddOrder a = itch::add_order(m);
                const std::uint32_t slot = syms_.intern(a.sym);
                last_sym_ = a.sym;
                orders_.insert(OrderTable::Entry{a.ref, slot, a.price, a.shares, a.buy});
                Book& b = books_[slot];
                if (a.buy) b.bid.add(a.price, a.shares); else b.ask.add(a.price, a.shares);
                return &b;
            }
            case 'E': case 'C': {
                auto* e = orders_.find(itch::exec_ref(m));
                if (!e) { ++st_.unknown_refs; return nullptr; }
                return reduce(e, itch::exec_shares(m));
            }
            case 'X': {
                auto* e = orders_.find(itch::cancel_ref(m));
                if (!e) { ++st_.unknown_refs; return nullptr; }
                return reduce(e, itch::cancel_shares(m));
            }
            case 'D': {
                auto* e = orders_.find(itch::delete_ref(m));
                if (!e) { ++st_.unknown_refs; return nullptr; }
                return reduce(e, e->shares);
            }
            case 'U': {
                auto* e = orders_.find(itch::replace_old(m));
                if (!e) { ++st_.unknown_refs; return nullptr; }
                // Replace = retire the old reference entirely, create a new one.
                const std::uint32_t slot = e->sym_slot;
                const bool buy = e->buy;
                Book& b = books_[slot];
                if (buy) b.bid.remove(e->price, e->shares); else b.ask.remove(e->price, e->shares);
                orders_.erase(e);

                const std::uint32_t np = itch::replace_price(m);
                const std::uint32_t ns = itch::replace_shares(m);
                orders_.insert(OrderTable::Entry{itch::replace_new(m), slot, np, ns, buy});
                if (buy) b.bid.add(np, ns); else b.ask.add(np, ns);
                return &b;
            }
            default: return nullptr;                  // 'P' trade: no book change
        }
    }

    const Stats& stats() const { return st_; }
    const Strategy& strategy() const { return strat_; }
    const unsigned char* out() const { return out_; }
    std::size_t out_len() const { return out_len_; }
    std::uint32_t symbols() const { return syms_.count(); }
    std::size_t live_orders() const { return orders_.size(); }

    // Keeps the optimiser honest: the harness consumes this, so no stage can be
    // dead-code eliminated.
    std::uint64_t checksum() const { return token_ ^ out_len_ ^ st_.book_events; }

    void prefault_state() {
        // Touch the output buffer; the big tables are touched by construction.
        for (auto& c : out_) c = 0;
    }

private:
    __attribute__((always_inline)) inline Book* reduce(OrderTable::Entry* e,
                                                       std::uint32_t shares) noexcept {
        Book& b = books_[e->sym_slot];
        const std::uint32_t n = shares < e->shares ? shares : e->shares;
        if (e->buy) b.bid.remove(e->price, n); else b.ask.remove(e->price, n);
        e->shares -= n;
        if (e->shares == 0) orders_.erase(e);         // fully done -> drop it
        return &b;
    }

    std::vector<Book> books_;
    SymbolTable       syms_;
    OrderTable        orders_;
    Strategy          strat_;
    Stats             st_;
    std::uint64_t     token_{0};
    std::uint64_t     last_sym_{0};
    unsigned char     out_[64]{};
    std::size_t       out_len_{0};
};

}  // namespace t2t
