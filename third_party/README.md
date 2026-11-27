# Vendored code

Both directories are copies of the author's own earlier projects, so the split
`tick2trade` runs the same decode/book/encode and the same risk gate as the
originals. Every local change is marked `shm-ring patch` in the source.

| dir | from | used for | local changes |
|---|---|---|---|
| `t2t/` | [tick2trade](https://github.com/Other-Now/tick2trade) | ITCH 5.0 decode, order book, OUCH 4.2 encode, `SymbolTable` | `Side::reserve` / `Book::reserve`, so the feed handler can pre-size price levels |
| `arg/` | [algo-risk-gateway](https://github.com/Other-Now/algo-risk-gateway) | pre-trade risk `Gate` (sanity, price bands, qty/value, position, self-trade, rate) | `kMaxInstr` 32 → 1024 (an equities universe, not an MCX book); `open_` / `own_bids` / `own_asks` moved to `std::pmr` containers on an `unsynchronized_pool_resource` (2 heap allocations per accepted order → 0; `-DSHMRING_GATE_HEAP` restores the original) |

`t2t/strategy.hpp` and `t2t/pipeline.hpp` are only included for `SymbolTable`; the
signal itself is re-implemented in `apps/t2t_core.hpp` with two fixes (per-symbol
state, and the OUCH symbol taken from the book that moved).
