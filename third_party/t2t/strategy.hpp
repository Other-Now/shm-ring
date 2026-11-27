#pragma once

// The decision stage.
//
// The strategy itself is deliberately trivial -- a queue-imbalance trigger with
// a spread guard. It is not the point of the project and pretending otherwise
// would be dishonest: no edge is being claimed here. What it has to be is
// *representative of the shape* of a real hot-path signal, namely branchy,
// reading only the touch, and carrying state across events.
//
// The one property that matters for the measurement is that it must not be
// optimised away. Every decision feeds an accumulator the harness consumes, so
// the compiler cannot delete the stage it is being asked to time.

#include <cstdint>

#include "t2t/book.hpp"

namespace t2t {

struct Signal {
    bool          send;
    bool          buy;
    std::uint32_t price;
    std::uint32_t shares;
};

class Strategy {
public:
    // imbalance_num/den express the trigger as a ratio without a divide:
    // fire when heavy*den > light*num.
    Strategy(std::uint32_t max_spread_ticks, std::uint32_t imbalance_num,
             std::uint32_t imbalance_den, std::uint32_t clip)
        : max_spread_(max_spread_ticks), num_(imbalance_num), den_(imbalance_den), clip_(clip) {}

    inline Signal on_book(const Book& b) noexcept {
        if (b.bid.empty() || b.ask.empty()) return {false, false, 0, 0};

        const Level& bid = b.bid.top();
        const Level& ask = b.ask.top();

        // Crossed or locked: the book is mid-update or the feed is inconsistent.
        // Never act on it.
        if (ask.price <= bid.price) { ++crossed_; return {false, false, 0, 0}; }

        const std::uint32_t spread = ask.price - bid.price;
        if (spread > max_spread_) return {false, false, 0, 0};

        // Queue imbalance: heavy side is where the resting size is.
        const std::uint64_t bs = bid.shares, as = ask.shares;
        const bool bid_heavy = bs * den_ > as * num_;
        const bool ask_heavy = as * den_ > bs * num_;
        if (!bid_heavy && !ask_heavy) return {false, false, 0, 0};

        // Only act on a *change* of state, or we would fire on every event in a
        // stable book -- which would make the measured send rate meaningless.
        const int state = bid_heavy ? 1 : -1;
        if (state == last_state_) return {false, false, 0, 0};
        last_state_ = state;

        ++fires_;
        // Join the heavy side at the touch.
        return bid_heavy ? Signal{true, true, bid.price, clip_}
                         : Signal{true, false, ask.price, clip_};
    }

    std::uint64_t fires() const { return fires_; }
    std::uint64_t crossed() const { return crossed_; }

private:
    std::uint32_t max_spread_, num_, den_, clip_;
    int           last_state_{0};
    std::uint64_t fires_{0}, crossed_{0};
};

}  // namespace t2t
