#pragma once

// The order-out stage: NASDAQ OUCH 4.2 EnterOrder encoding.
//
// This closes the loop that the market-data side opens. ITCH is what the
// exchange says to you; OUCH is what you say back. A tick-to-trade number that
// stops at "the strategy decided" is not tick-to-trade -- the bytes have to be
// on the wire, so the encode belongs inside the measured region.
//
// Encoding into a caller-supplied buffer, big-endian, field by field. No
// serialisation library, no stream, no allocation: 49 bytes written straight
// into a pre-touched buffer that stays hot in L1.

#include <cstdint>
#include <cstring>

namespace t2t::ouch {

// OUCH 4.2 EnterOrder: 49 bytes on the wire.
constexpr std::size_t kEnterOrderLen = 49;

inline void store_be16(unsigned char* p, std::uint16_t v) {
    v = __builtin_bswap16(v); std::memcpy(p, &v, 2);
}
inline void store_be32(unsigned char* p, std::uint32_t v) {
    v = __builtin_bswap32(v); std::memcpy(p, &v, 4);
}
inline void store_be64(unsigned char* p, std::uint64_t v) {
    v = __builtin_bswap64(v); std::memcpy(p, &v, 8);
}

struct EnterOrder {
    std::uint64_t token;    // client order token (14 bytes on the wire, we use 8 + pad)
    std::uint64_t symbol;   // 8 ASCII bytes, already packed
    std::uint32_t shares;
    std::uint32_t price;
    bool          buy;
};

// Returns bytes written. `dst` must have room for kEnterOrderLen.
inline std::size_t encode(unsigned char* dst, const EnterOrder& o) noexcept {
    unsigned char* p = dst;
    *p++ = 'O';                                  //  0: message type
    // 1..14: order token, 14 ASCII bytes. Encode the numeric token big-endian in
    // the low 8 and space-pad the rest -- stable, comparable, and no itoa on the
    // hot path.
    std::memset(p, ' ', 6); p += 6;
    store_be64(p, o.token); p += 8;
    *p++ = o.buy ? 'B' : 'S';                    // 15: buy/sell indicator
    store_be32(p, o.shares); p += 4;             // 16: shares
    store_be64(p, o.symbol); p += 8;             // 20: stock symbol (8 bytes)
    store_be32(p, o.price); p += 4;              // 28: price
    store_be32(p, 99999);   p += 4;              // 32: time in force (99999 = day)
    store_be32(p, 0);       p += 4;              // 36: firm (0 = default)
    *p++ = 'P';                                  // 40: display = price to comply
    *p++ = 'Y';                                  // 41: capacity
    *p++ = 'Y';                                  // 42: intermarket sweep eligibility
    store_be32(p, 0); p += 4;                    // 43: minimum quantity
    *p++ = 'N';                                  // 47: cross type
    *p++ = 'N';                                  // 48: customer type
    return static_cast<std::size_t>(p - dst);
}

}  // namespace t2t::ouch
