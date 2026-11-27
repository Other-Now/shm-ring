#pragma once

// tick2trade, split at the book.
//
//   FeedCore      ITCH 5.0 decode + order book (tick2trade's itch/book code)
//                 -> one TobUpdate per book-moving message
//   StrategyCore  queue-imbalance signal (tick2trade's rules)
//                 -> ObjectPool<Order> -> algo-risk-gateway Gate::submit
//                 -> OUCH 4.2 EnterOrder encode
//
// The same two classes run either in one process (t2t_split inproc: a direct
// function call between them) or in two (feed -> SpscRing in shm -> strategy),
// so the only thing that differs between the two latency numbers is the
// process boundary.
//
// Differences from tick2trade's single-process Pipeline, both deliberate:
//  * the OUCH symbol is the symbol of the book that moved (Pipeline used the
//    symbol of the most recent *add*, which is wrong for executes/cancels);
//  * the signal's "last state" is per symbol (Pipeline kept one global state).

#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <vector>

#include "arg/gate.hpp"
#include "shmring/shmring.hpp"
#include "t2t/book.hpp"
#include "t2t/itch.hpp"
#include "t2t/ouch.hpp"
#include "t2t/pipeline.hpp"  // SymbolTable

namespace t2ts {

constexpr std::uint32_t kMaxSymbols = 1u << 14;  // == SymbolTable capacity
constexpr std::uint32_t kEndOfFeed = 0xffffffffu;

// What crosses the process boundary: one book change, top of book only.
// Exactly one cache line, so one message = one line transfer.
struct alignas(64) TobUpdate {
    std::uint64_t t_rx;     // TSC when the feed handler started decoding the message
    std::uint64_t t_push;   // TSC just before the ring push (after book + snapshot)
    std::uint64_t itch_ts;  // exchange timestamp, ns since midnight
    std::uint64_t sym;      // 8 ASCII bytes, packed
    std::uint32_t slot;     // dense symbol index, or kEndOfFeed
    std::uint32_t bid_px, bid_sz, ask_px, ask_sz;  // 0 = side empty
    std::uint32_t flags;
    std::uint64_t pad;
};
static_assert(sizeof(TobUpdate) == 64);
static_assert(shmring::ShmPayload<TobUpdate>);

// Latest top of book per symbol, for the risk check's reference price.
struct Tob {
    std::uint32_t bid_px, bid_sz, ask_px, ask_sz;
};
static_assert(sizeof(Tob) % 8 == 0);

using TobRing = shmring::Ring<TobUpdate, 4096, shmring::Producers::One>;
using TobTable = shmring::SnapshotTable<Tob, kMaxSymbols>;

// ------------------------------------------------------------------- feed

class FeedCore {
public:
    // reserve_levels: pre-size every price-level vector so steady state does
    // not grow them. Off = tick2trade's original behaviour (see
    // results/allocations.md for what that costs).
    explicit FeedCore(bool reserve_levels = true) : books_(kMaxSymbols), slot_sym_(kMaxSymbols, 0) {
        if (reserve_levels)
            for (auto& b : books_) b.reserve(kLevelReserve);
    }

    static constexpr std::size_t kLevelReserve = 64;

    // Decode one ITCH message; if a book moved, fill `out` and return true.
    bool on_message(const unsigned char* m, std::uint16_t len, std::uint64_t t_rx, TobUpdate& out) noexcept {
        ++messages_;
        const char type = static_cast<char>(m[0]);
        const int expect = t2t::itch::wire_len(type);
        if (expect < 0 || int(len) != expect) return false;
        const std::uint32_t slot = apply(type, m);
        if (slot == kEndOfFeed) return false;
        ++book_events_;
        const t2t::Book& b = books_[slot];
        out.t_rx = t_rx;
        out.itch_ts = t2t::itch::msg_ts(m);
        out.sym = slot_sym_[slot];
        out.slot = slot;
        out.bid_px = b.bid.empty() ? 0 : b.bid.top().price;
        out.bid_sz = b.bid.empty() ? 0 : b.bid.top().shares;
        out.ask_px = b.ask.empty() ? 0 : b.ask.top().price;
        out.ask_sz = b.ask.empty() ? 0 : b.ask.top().shares;
        out.flags = 0;
        return true;
    }

    std::uint64_t messages() const { return messages_; }
    std::uint64_t book_events() const { return book_events_; }
    std::uint64_t unknown_refs() const { return unknown_refs_; }

private:
    std::uint32_t apply(char type, const unsigned char* m) noexcept {
        using namespace t2t;
        switch (type) {
            case 'A': case 'F': {
                const itch::AddOrder a = itch::add_order(m);
                const std::uint32_t slot = syms_.intern(a.sym);
                slot_sym_[slot] = a.sym;
                orders_.insert(OrderTable::Entry{a.ref, slot, a.price, a.shares, a.buy});
                Book& b = books_[slot];
                if (a.buy) b.bid.add(a.price, a.shares); else b.ask.add(a.price, a.shares);
                return slot;
            }
            case 'E': case 'C': return reduce(orders_.find(itch::exec_ref(m)), itch::exec_shares(m));
            case 'X': return reduce(orders_.find(itch::cancel_ref(m)), itch::cancel_shares(m));
            case 'D': {
                auto* e = orders_.find(itch::delete_ref(m));
                return reduce(e, e ? e->shares : 0);
            }
            case 'U': {
                auto* e = orders_.find(itch::replace_old(m));
                if (!e) { ++unknown_refs_; return kEndOfFeed; }
                const std::uint32_t slot = e->sym_slot;
                const bool buy = e->buy;
                Book& b = books_[slot];
                if (buy) b.bid.remove(e->price, e->shares); else b.ask.remove(e->price, e->shares);
                orders_.erase(e);
                const std::uint32_t np = itch::replace_price(m), ns = itch::replace_shares(m);
                orders_.insert(OrderTable::Entry{itch::replace_new(m), slot, np, ns, buy});
                if (buy) b.bid.add(np, ns); else b.ask.add(np, ns);
                return slot;
            }
            default: return kEndOfFeed;  // 'P' trade: no book change
        }
    }

    std::uint32_t reduce(t2t::OrderTable::Entry* e, std::uint32_t shares) noexcept {
        if (!e) { ++unknown_refs_; return kEndOfFeed; }
        t2t::Book& b = books_[e->sym_slot];
        const std::uint32_t n = shares < e->shares ? shares : e->shares;
        if (e->buy) b.bid.remove(e->price, n); else b.ask.remove(e->price, n);
        e->shares -= n;
        const std::uint32_t slot = e->sym_slot;
        if (e->shares == 0) orders_.erase(e);
        return slot;
    }

    std::vector<t2t::Book> books_;
    std::vector<std::uint64_t> slot_sym_;
    t2t::SymbolTable syms_{kMaxSymbols};
    t2t::OrderTable orders_;
    std::uint64_t messages_{0}, book_events_{0}, unknown_refs_{0};
};

// --------------------------------------------------------------- strategy

struct Order {
    std::uint64_t token;
    std::uint64_t sym;
    std::uint32_t price, shares;
    bool buy;
    std::uint16_t instr;
};

// Risk limits for the gate. The gate has a fixed instrument table (raised from
// 32 to 1024 in the vendored copy); the strategy whitelists the first 1023
// symbols that signal and anything beyond is rejected as BAD_INSTRUMENT -- a
// whitelist is what a real pre-trade gate enforces anyway.
inline arg::Config risk_config() {
    arg::Config c = arg::zeroed();
    for (int i = 1; i < arg::kMaxInstr; ++i) {
        arg::InstrCfg& ic = c.instr[i];
        ic.enabled = 1;
        arg::set_symbol(ic, "ITCH");
        ic.tick = 100;                 // ITCH prices have 4 implied decimals: 100 = 1 cent
        ic.band_lo = 100;              // static band: $0.01 .. $200,000
        ic.band_hi = 2'000'000'000;
        ic.ref_price = 0;              // replaced by the live mid before every check
        ic.ltp_band_bp = 500;          // +/- 5% from the current mid
        ic.max_qty = 1000;
        ic.max_value = 5'000'000'000'000;
        ic.max_pos = 1'000'000'000;
    }
    c.algo[1] = arg::AlgoCfg{1, 1, 100101, 100'000, 10'000};  // registered, 100k orders/s
    c.unregistered_ops_cap = 10;
    c.otr = arg::OtrCfg{0xffffffffu, 0, 0};  // no fills in a replay, so OTR is meaningless: off
    c.loss_limit = 0;
    return c;
}

struct StratStats {
    std::uint64_t updates{0}, signals{0}, accepted{0}, rejected{0}, pool_empty{0};
    std::uint64_t reject_by_reason[std::size_t(arg::Reason::kCount)]{};
    std::uint64_t checksum{0};  // over every encoded OUCH byte: inproc and xproc must agree
};

class StrategyCore {
public:
    // tick2trade's parameters: <=5c spread, 3:1 imbalance, 100-share clip.
    StrategyCore() : state_(kMaxSymbols, 0), instr_of_(kMaxSymbols, -1), gate_(risk_config()) {}

    // Returns the encoded length (>0) if an order went out; t_out is stamped
    // after the encode, which is the end of the tick-to-order path.
    // use_snapshot=false takes the reference price from the update itself,
    // which saves reading a second cache line written by the other process.
    explicit StrategyCore(bool use_snapshot) : StrategyCore() { use_snapshot_ = use_snapshot; }

    std::size_t on_update(const TobUpdate& u, const TobTable& snaps, std::uint64_t& t_out) noexcept {
        ++st_.updates;
        if (u.bid_sz == 0 || u.ask_sz == 0) return 0;
        if (u.ask_px <= u.bid_px) return 0;               // crossed/locked: never act
        if (u.ask_px - u.bid_px > kMaxSpread) return 0;
        const std::uint64_t bs = u.bid_sz, as = u.ask_sz;
        const bool bid_heavy = bs * kDen > as * kNum, ask_heavy = as * kDen > bs * kNum;
        if (!bid_heavy && !ask_heavy) return 0;
        const std::int8_t s = bid_heavy ? 1 : -1;
        if (state_[u.slot] == s) return 0;                // act on a change of state only
        state_[u.slot] = s;
        ++st_.signals;

        Order* o = pool_.acquire(Order{++token_, u.sym, bid_heavy ? u.bid_px : u.ask_px, kClip, bid_heavy,
                                       instr_for(u.slot)});
        if (!o) { ++st_.pool_empty; return 0; }

        // Reference price for the LTP band: the latest snapshot, which may be
        // newer than this update -- that is the point of reading it separately.
        const Tob ref = use_snapshot_ ? snaps[u.slot].load() : Tob{u.bid_px, u.bid_sz, u.ask_px, u.ask_sz};
        if (o->instr > 0 && ref.bid_px && ref.ask_px)
            gate_.on_market(o->instr, (std::int64_t(ref.bid_px) + ref.ask_px) / 2, u.itch_ts);

        const arg::NewOrder no{o->token, 1, o->instr, o->buy ? arg::Side::Buy : arg::Side::Sell, arg::Tif::Ioc,
                               std::int64_t(o->price), std::int64_t(o->shares)};
        const arg::Decision d = gate_.submit(no, u.itch_ts);
        std::size_t n = 0;
        if (d.ok()) {
            n = t2t::ouch::encode(out_, t2t::ouch::EnterOrder{o->token, o->sym, o->shares, o->price, o->buy});
            t_out = shmring::rdtsc();
            ++st_.accepted;
            for (std::size_t i = 0; i < n; ++i) st_.checksum = (st_.checksum ^ out_[i]) * 1099511628211ull;
            gate_.on_done(o->token, u.itch_ts);  // IOC: no exchange here, so it is done at once
        } else {
            ++st_.rejected;
            ++st_.reject_by_reason[std::size_t(d.reason)];
        }
        pool_.release(o);
        return n;
    }

    const StratStats& stats() const { return st_; }

private:
    static constexpr std::uint32_t kMaxSpread = 500, kNum = 3, kDen = 1, kClip = 100;

    std::uint16_t instr_for(std::uint32_t slot) noexcept {
        if (instr_of_[slot] < 0) instr_of_[slot] = next_instr_ < arg::kMaxInstr ? next_instr_++ : 0;
        return std::uint16_t(instr_of_[slot]);  // 0 = not whitelisted -> BAD_INSTRUMENT
    }

    std::vector<std::int8_t> state_;
    std::vector<std::int16_t> instr_of_;
    std::int16_t next_instr_{1};
    bool use_snapshot_{true};
    shmring::ObjectPool<Order, 64> pool_;
    arg::Gate gate_;
    std::uint64_t token_{0};
    unsigned char out_[64]{};
    StratStats st_;
};

}  // namespace t2ts
