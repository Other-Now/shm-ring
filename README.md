# shm-ring

Lock-free shared-memory IPC for C++20: SPSC and MPSC rings over
`shm_open` + `mmap`, a seqlock for top-of-book snapshots, an object pool, and a
compile-time queue choice through concepts. Used to split
[tick2trade](https://github.com/Other-Now/tick2trade) into a feed-handler
process and a strategy process, with the
[algo-risk-gateway](https://github.com/Other-Now/algo-risk-gateway) pre-trade
gate in the order path.

Header-only. Linux, x86-64, GCC 13 / Clang. CI: GCC + Clang, TSan (with a
planted-race negative control), ASan+UBSan, Valgrind steady-state allocation
check, and a bench smoke run with 2 MiB huge pages reserved.

## Results

Intel i5-9300H laptop (4C/8T, Coffee Lake) running **WSL2**: 4 vCPUs, GCC 13.3, `-O3 -march=native`.
Everything is a median of repeated, interleaved runs. Raw data and the script are in
[`results/raw/local-wsl/`](results/raw/local-wsl) and [`tools/run_all.sh`](tools/run_all.sh).
Read [Limits](#limits-read-before-quoting) before quoting a tail number.

### 1. Cross-process one-way latency (ping-pong, RTT/2, pinned cores 1 and 3)

| transport | p50 | p99 | p99.9 |
|---|---|---|---|
| **shm SPSC ring** | **72 ns** | **96 ns** | **124 ns** |
| eventfd, busy-poll | 1,113 ns | 2,537 ns | 12.5 µs |
| pipe, busy-poll | 1,236 ns | 2,531 ns | 12.1 µs |
| Unix domain socket, busy-poll | 2,120 ns | 5,578 ns | 16.8 µs |
| eventfd, blocking read | 27.3 µs | 55.7 µs | 133 µs |
| pipe, blocking read | 27.7 µs | 53.5 µs | 145 µs |
| Unix domain socket, blocking read | 27.6 µs | 54.2 µs | 149 µs |

The ring is ~15× faster than the best syscall transport even when that transport busy-polls,
and ~380× faster than the normal blocking use. All three kernel transports block at the same
~27 µs. That figure is the scheduler/hypervisor wake-up and has little to do with the transport.
In 10 extra back-to-back shm sessions the p50 was 68–84 ns in 8 of them. The other 2 measured
~20 ns, when Hyper-V happened to place both vCPUs on one physical core
([`shm_sessions.csv`](results/raw/local-wsl/shm_sessions.csv)). The 72 ns figure is the cross-core case.

### 2. False sharing: padding × cached index (two processes, 20M msgs, 7 runs, `perf stat`)

| SPSC layout | M msg/s | ns/msg | memory-ordering machine clears / 1k msgs | HITM / 1k msgs | IPC |
|---|---|---|---|---|---|
| `Packed`: head+tail on one line | 78.1 | 12.8 | 408 | 64 | 0.48 |
| `Padded`: separate lines | 70.1 | 14.3 | 421 | 376 | 0.46 |
| `PackedCached`: cached index, still one line | 66.7 | 15.0 | 433 | 95 | 0.39 |
| **`PaddedCached`** | **151.1** | **6.6** | **98** | 136 | **0.97** |

- **Padding alone did nothing.** In a plain SPSC ring each side reads the other's index on
  every operation, so that line is *truly* shared. Splitting it just makes two lines ping-pong
  instead of one, and HITMs rose 64 → 376 per 1k msgs.
- **Caching the other index alone also did nothing** (66.7), because `tail` still shares the
  producer's line. Every consumer store invalidates a line the producer no longer needs. That
  is genuine false sharing.
- **Both together: 1.9× throughput and 4× fewer memory-ordering pipeline flushes.** The
  remaining ~125 HITM per 1k msgs is the floor of one slot line per 8 messages (8-byte
  messages, 64-byte lines).
- `perf c2c` needs `perf mem` (PEBS load-latency sampling), which WSL2 doesn't expose. So these
  are the raw counters: `mem_load_l3_hit_retired.xsnp_hitm`, the HITM event c2c is built on,
  plus `machine_clears.memory_ordering`. HITM alone didn't track throughput; machine clears did.

MPSC (Vyukov) with 1 / 2 / 3 producer processes: 46.3 / 24.8 / 16.6 M msg/s, with 0 ordering
errors. The CAS on the shared ticket counter serialises producers.

### 3. tick2trade split: tick-to-order across the process boundary

First 3M messages of the real NASDAQ ITCH 5.0 sample: 2.78M book updates and 12,860 orders
through the risk gate. Book events are paced 2 µs apart; unpaced, the feed outruns the strategy
and you measure the queue. Median of 5 runs.

| | tick-to-order p50 | p99 | p99.9 | feed: decode+book+snapshot | ring hop | strategy + risk + OUCH |
|---|---|---|---|---|---|---|
| one process (direct call) | 347 ns | 1.2 µs | 3.1 µs | 119 ns | – | 198 ns |
| **two processes (shm ring)** | **569 ns** | 7.6 µs | 127 µs | 124 ns | **106 ns** | 288 ns |

- The process boundary costs **~220 ns at the median**: ~106 ns of ring hop plus ~90 ns of
  strategy work that runs colder on the other core.
- Ring transit over *all* 2.78M updates: p50 125 ns. The ring hop is stable; the cross-process
  **tail is not the ring**. Pinned and spinning, a WSL2 vCPU still disappears ~2,000 times a
  second (gap p50 ~10 µs, p99 ~115–150 µs, max 10–14 ms; [`hiccup`](bench/hiccup.cpp)). Each stall
  of the strategy process queues every event behind it; in one process a stall delays only the
  current event.
- **Parity**: one-process and two-process runs emit byte-identical OUCH (same checksum) in every
  paced run. ctest and CI check this.
- Reading the risk reference price from the seqlock snapshot instead of from the update made
  no measurable difference (569 vs 605 ns, within run-to-run noise).

### 4. Allocations in steady state

| | strategy process | feed process | memcheck errors |
|---|---|---|---|
| Valgrind, extra allocations from message 1M to 2M, **original risk gate** | **+9,934** (2 per order) | +2 | 0 |
| Valgrind, same, **pmr risk gate** | **+0** | +2 | 0 |
| `operator new` counter, 3M msgs, original gate / no book reserve | 25,716 | 4,389 | |
| `operator new` counter, 3M msgs, final | 2 (one-time pool growth) | 3 (books deeper than 64 levels) | |

`SHMRING_ALLOC_TRACE=1` prints a backtrace for every steady-state allocation, and that located both
offenders:

- **algo-risk-gateway**: its self-trade price map (`std::map`) and open-order map
  (`std::unordered_map`) allocated a node per accepted order. Both containers now use `std::pmr`
  on an `unsynchronized_pool_resource`, which recycles freed nodes.
- **tick2trade**: price-level vectors grew in place; they now reserve 64 levels per side.

The claim is **zero allocations per message**, not zero ever. Five one-time growth events remain
over 3M messages. Valgrind needs `--soname-synonyms=somalloc=nouserintercepts`, because the
binary replaces `operator new` itself.

### 5. Microbenchmarks (Google Benchmark, one core, no cache-line transfer)

| | ns | | ns |
|---|---|---|---|
| SPSC push+pop | 5.6–5.9 | object pool acquire+release | 7.4 |
| MPSC push+pop | 10.2 | `new` + `delete` | 15.9 |
| seqlock store | 1.4 | pool, 256 live, recycle oldest | 2.3 |
| seqlock load | 4.8 | heap, same pattern | 16.3 |
| push+pop: direct / concept-generic / **virtual** | 3.10 / 3.08 / 3.26 | SPSC burst-64, padded vs padded+cached | 208 vs 198 |

The concept-generic code costs the same as hand-written code. A virtual interface was also
within noise here: a perfectly predicted indirect call is nearly free when the body is a few ns
of dependent memory operations. What the template actually buys is that the call can be inlined
into the caller.

### 6. Correctness

- Unit and stress tests: 1,014,057 checks, covering SPSC (4 layouts), MPSC, seqlock torture
  (0 torn reads), cross-process SPSC/MPSC over `fork` with separate mappings, wrong-type attach
  rejected, and pool/ring allocation-free.
- **ThreadSanitizer clean**, with a **negative control**: [`tests/tsan_canary.cpp`](tests/tsan_canary.cpp)
  is the ring with `release` weakened to `relaxed`. That compiles to the same x86 instructions,
  and TSan must report the race (CI fails if it doesn't).
- ASan+UBSan clean.
- Huge pages: CI reserves 64 × 2 MiB pages and checks that `/proc/self/smaps` reports
  `KernelPageSize: 2048 kB` for the ring. The laptop has none reserved and no root.

### 7. Second machine: the GitHub Actions VM (smoke run, single runs, noisy)

The `bench-smoke` CI job reproduces the **shape** of the results on a different host (4-vCPU
Azure VM, pinned to vCPUs 1 and 3):

- **False-sharing 2×2**: packed 163, padded 195, packed+cached 144, **padded+cached 410** M msg/s.
  Same ordering, and the cached-but-packed layout is again the worst.
- **Wake-ups**: blocking pipe/UDS/eventfd take 1.5–3.5 µs there vs ~27 µs under WSL2, which
  confirms the WSL2 wake-up inflation noted above. Busy-poll eventfd/pipe take ~0.3 µs.
- **shm**: ~28 ns one-way (4 KiB pages) and ~23 ns (2 MiB pages, 3 runs each). That is too fast
  for a cross-core transfer, so those vCPUs are probably hyperthreads of one core. Don't read the
  5 ns as a huge-page effect.
- **Jitter**: ~1,900 gaps/s there too, but max 176 µs vs 10–14 ms under WSL2.

## What's in it

| | file | notes |
|---|---|---|
| shm segment | [`shm_segment.hpp`](include/shmring/shm_segment.hpp) | `shm_open`/`ftruncate`/`mmap(MAP_SHARED\|MAP_POPULATE)`; optional 2 MiB pages via hugetlbfs, verified from `/proc/self/smaps` |
| typed publish/attach | [`channel.hpp`](include/shmring/channel.hpp) | placement-new into the segment, header published last with a release store; attach checks size/align/type tag |
| SPSC ring | [`spsc_ring.hpp`](include/shmring/spsc_ring.hpp) | acquire/release on two indices, no CAS; `Layout` template parameter = padding × cached-index 2×2 |
| MPSC ring | [`mpsc_ring.hpp`](include/shmring/mpsc_ring.hpp) | Vyukov bounded queue, per-cell sequence numbers, CAS only on the producer side |
| seqlock | [`seqlock.hpp`](include/shmring/seqlock.hpp) | payload as `atomic<u64>` words (race-free, TSan-clean), no standalone fences |
| concepts | [`concepts.hpp`](include/shmring/concepts.hpp) | `ShmPayload`, `Queue`; `Ring<T, N, Producers::One\|Many>` picks the ring type |
| object pool | [`object_pool.hpp`](include/shmring/object_pool.hpp) | placement new into reserved storage, index free list, `nullptr` when exhausted |
| alloc counter | [`alloc_counter.hpp`](include/shmring/alloc_counter.hpp) | counts global `operator new`; `SHMRING_ALLOC_TRACE=1` prints a backtrace per steady-state allocation |
| tick2trade split | [`apps/`](apps) | `feed` / `strategy` / `run` (both) / `inproc` (same code, one process) |
| benchmarks | [`bench/`](bench) | `ipc_latency` (shm vs pipe/UDS/eventfd), `ring_throughput` (layouts, MPSC), `hiccup` (platform jitter), `micro` (Google Benchmark) |

```cpp
#include "shmring/shmring.hpp"
using namespace shmring;

struct Tick { std::uint64_t ts; std::uint32_t px, qty; };
using TickRing = Ring<Tick, 4096, Producers::One>;      // -> SpscRing<Tick, 4096>

// process A
auto out = Channel<TickRing>::create("ticks");
out->push(Tick{1, 100, 5});

// process B
auto in = Channel<TickRing>::attach("ticks");            // waits for A, validates the type
Tick t;
while (!in->try_pop(t)) cpu_relax();
```

## Build, test, reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build                 # unit + cross-process + inproc/xproc parity

# everything behind results/ (needs perf and valgrind; see the script header)
tools/run_all.sh build /path/to/20190130.NASDAQ_ITCH50 mytag
```

`data/sample.NASDAQ_ITCH50` is the synthetic 135k-message feed from tick2trade
(it runs on clone). The real numbers use NASDAQ's public
`01302019.NASDAQ_ITCH50` sample, first 3M messages (pre-market only; the file
is a prefix of the day).

## Limits (read before quoting)

- **One machine, under a hypervisor.** WSL2 adds vCPU preemption (~2k stalls/s), which dominates
  every tail above p99, and it inflates blocking wake-ups. Bare-metal tails would be very different;
  none were measured.
- **No SMT-vs-cross-core claim.** `lscpu` says vCPUs 2,3 are siblings, but they measured the same as
  1,3. Hyper-V doesn't pin vCPUs to host cores, which is also why 2 of 10 sessions saw ~20 ns.
- **`perf c2c` was not run.** WSL2 has no `perf mem`; the HITM and machine-clear counters were used
  instead.
- **Huge pages: functional check only (CI).** No latency claim. A 64 KiB ring fits in the TLB anyway.
- **MPSC liveness**: a producer killed between claiming a cell and publishing it blocks the
  consumer forever. That needs a heartbeat/poison protocol, which isn't built. The same applies to
  a dead consumer in SPSC: the producer sees `try_push` fail and nothing more.
- **The seqlock snapshot is "latest value".** When the strategy lags, the risk check sees a newer
  price than the update in hand, so decisions become timing-dependent (seen unpaced and under
  Valgrind). The parity test therefore uses `--no-snapshot`.
- **The strategy is tick2trade's toy queue-imbalance signal.** No edge is claimed. The risk gate's
  OTR check is disabled, because a replay has no fills. Orders are IOC and marked done at once,
  since there is no exchange.
- **The feed is the first 3M messages of the public NASDAQ sample**, which is pre-market only.
