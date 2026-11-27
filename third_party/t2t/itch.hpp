#pragma once

// Zero-copy NASDAQ ITCH 5.0 decode, restricted to the messages that move a book.
//
// "Zero-copy" here means literally that: no message is ever memcpy'd out of the
// mapped file. Fields are loaded straight from the mapped bytes with a
// byte-swapping load. The decoder is a set of accessors over a const pointer,
// so the compiler folds a field read into a single ldr+rev.

#include <cstdint>
#include <cstring>

namespace t2t::itch {

// ---------------------------------------------------------------------------
// Big-endian loads. ITCH is network byte order; every field needs a swap on
// both ARM and x86. memcpy is the only strictly-conforming way to do an
// unaligned load, and every compiler lowers this pair to one instruction.
// ---------------------------------------------------------------------------

inline std::uint16_t be16(const unsigned char* p) {
    std::uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v);
}
inline std::uint32_t be32(const unsigned char* p) {
    std::uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v);
}
inline std::uint64_t be64(const unsigned char* p) {
    std::uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v);
}
// 6-byte big-endian (ITCH timestamps are 48-bit nanoseconds since midnight).
inline std::uint64_t be48(const unsigned char* p) {
    return (std::uint64_t(be16(p)) << 32) | std::uint64_t(be32(p + 2));
}

enum class Type : char {
    AddOrder            = 'A',
    AddOrderMPID        = 'F',
    OrderExecuted       = 'E',
    OrderExecutedPrice  = 'C',
    OrderCancel         = 'X',
    OrderDelete         = 'D',
    OrderReplace        = 'U',
    Trade               = 'P',
};

// Fixed wire lengths, used to validate framing against the declared length.
inline int wire_len(char t) {
    switch (t) {
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        default:  return -1;   // not a book-moving message
    }
}

// A stock symbol is 8 right-padded ASCII bytes; pack it into a u64 so symbol
// comparison and hashing are single-register operations rather than strcmp.
inline std::uint64_t sym_key(const unsigned char* p) {
    std::uint64_t v; std::memcpy(&v, p, 8); return v;
}

struct AddOrder {
    std::uint64_t ref;
    std::uint64_t sym;
    std::uint32_t shares;
    std::uint32_t price;   // fixed point, 4 implied decimals
    bool          buy;
};

// Accessors. Offsets are from the ITCH 5.0 spec, message body starting at the
// type byte (offset 0), matching NASDAQ BinaryFILE framing after the length.
inline AddOrder add_order(const unsigned char* m) {
    return AddOrder{
        be64(m + 11),                 // order reference number
        sym_key(m + 24),              // stock
        be32(m + 20),                 // shares
        be32(m + 32),                 // price
        m[19] == 'B',                 // buy/sell indicator
    };
}
inline std::uint64_t exec_ref(const unsigned char* m)   { return be64(m + 11); }
inline std::uint32_t exec_shares(const unsigned char* m){ return be32(m + 19); }
inline std::uint64_t cancel_ref(const unsigned char* m) { return be64(m + 11); }
inline std::uint32_t cancel_shares(const unsigned char* m){ return be32(m + 19); }
inline std::uint64_t delete_ref(const unsigned char* m) { return be64(m + 11); }
inline std::uint64_t replace_old(const unsigned char* m){ return be64(m + 11); }
inline std::uint64_t replace_new(const unsigned char* m){ return be64(m + 19); }
inline std::uint32_t replace_shares(const unsigned char* m){ return be32(m + 27); }
inline std::uint32_t replace_price(const unsigned char* m) { return be32(m + 31); }
inline std::uint64_t msg_ts(const unsigned char* m)     { return be48(m + 5); }

}  // namespace t2t::itch
