# shm-ring — study notes

A 1–2 day read. Every section points at the file that implements it. Numbers
quoted here come from `results/`; re-read those before quoting anything.

## 0. The 30-second version

Two processes share a file in `/dev/shm`, mapped into both address spaces.
Inside it sits a ring buffer whose only synchronisation is two integers: the
producer writes a slot then publishes `head` with a *release* store; the
consumer *acquire*-loads `head`, reads the slot, publishes `tail`. No syscalls,
no locks, no kernel on the data path — so the cost of a message is the cost of
moving a couple of cache lines between cores (~72 ns one-way p50 on this
laptop, vs ~27 µs for a blocking pipe/socket/eventfd under WSL2).

## 1. Shared memory segment — `include/shmring/shm_segment.hpp`

- `shm_open(name, O_CREAT|O_EXCL|O_RDWR)` creates a file in `/dev/shm` (tmpfs).
  `ftruncate` sizes it, `mmap(MAP_SHARED)` maps it. Another process
  `shm_open`s the same name and maps the same physical pages.
- **`MAP_POPULATE`** pre-faults every page at map time. Without it the first
  touch of each 4 KiB page is a page fault on the hot path.
- **Creator unlinks a stale segment first.** After a crash the name still
  exists; `O_EXCL` would fail, and an attacher might map stale data.
- **No pointers inside the segment.** Each process maps it at a different
  virtual address. The rings use indices only. (`attach_validation` test maps
  the same segment twice in one process and moves data between the mappings.)
- **Huge pages.** `MAP_HUGETLB` only works for anonymous memory or hugetlbfs
  files, *not* a tmpfs fd. So shared huge pages = a file in `/dev/hugepages`,
  which needs `vm.nr_hugepages > 0` (root). `HugePages::Try` falls back to tmpfs;
  `page_size_kb()` reads `/proc/self/smaps` to prove which one you got. The
  laptop (WSL2, no root, 0 pages reserved) can't use them; CI reserves 64 pages
  and checks the path. For a 64 KiB ring the TLB already covers it — huge pages
  matter for large tables, not for this ring. Say that if asked.

## 2. Publishing a typed object safely — `include/shmring/channel.hpp`

- `Shared<Obj>::create`: map, placement-new a `Header`, placement-new `Obj`,
  fill header, **then** `magic.store(kMagic, release)`.
- `attach`: map, spin until `magic.load(acquire) == kMagic`, then check
  version / `sizeof` / `alignof` / a type tag (FNV hash of
  `__PRETTY_FUNCTION__`). Attaching `SpscRing<Foo,1024>` to a segment holding
  `SpscRing<Bar,4096>` throws instead of corrupting memory.
- Why release/acquire on `magic`: otherwise the attacher could see `magic` set
  before the constructor's writes to the ring are visible.

## 3. SPSC ring — `include/shmring/spsc_ring.hpp`

```
push: h = head (relaxed, I own it)
      if h - tail == N: full            (tail: acquire)
      slot[h & mask] = v
      head.store(h + 1, release)        <- publishes the slot write
pop:  t = tail (relaxed, I own it)
      if t == head: empty               (head: acquire)
      v = slot[t & mask]
      tail.store(t + 1, release)        <- tells producer the slot is free
```

- **Why release on head**: all writes before it (the slot) become visible to
  whoever acquire-loads the new head. Without it the consumer may read a stale
  slot. On x86 (TSO) release/acquire are plain MOVs — the ordering costs
  nothing at runtime, but it constrains the *compiler*, and it is what makes
  the program correct on ARM. `tests/tsan_canary.cpp` is this ring with
  `relaxed` instead of `release`: identical x86 machine code, and TSan reports
  the data race. That's the proof TSan isn't vacuous.
- **Free-running 64-bit indices**: `head - tail` is the size; no "one empty
  slot" trick; they never wrap in practice (2^64 messages).
- **Capacity power of two**: `& mask` instead of `%`.
- **`is_always_lock_free` static_assert**: a lock-based `std::atomic` would put
  a process-local mutex into shared memory — broken across processes.

### Layouts and the false-sharing experiment

`Layout` is a template parameter → a 2×2 of padding × cached index:

| | index re-read every op | private cached copy of the other index |
|---|---|---|
| head+tail on one line | `Packed` (true sharing) | `PackedCached` (**false** sharing) |
| separate lines | `Padded` | `PaddedCached` (the fix) |

- **True vs false sharing.** In an uncached SPSC ring each side *reads the
  other side's index on every op*. The index line genuinely must move —
  padding can't help, and measured it didn't (`Padded` ≈ `Packed`; padding
  actually *raised* HITMs because two lines now ping-pong instead of one).
- Measured: Packed 78, Padded 70, PackedCached 67, PaddedCached 151 M msg/s;
  machine clears (memory ordering) 408 -> 98 per 1k msgs. Neither fix alone helps.
- **Cached index** (Rigtorp's trick): the producer keeps `tail_cache` and only
  re-reads `tail` when the ring looks full; the consumer keeps `head_cache` and
  only re-reads `head` when the ring looks empty. Now the producer does *not*
  need the consumer's line on most ops — so if `tail` still lives on the
  producer's line (`PackedCached`), every consumer store invalidates it for
  nothing. **That** is false sharing, and padding fixes it.
- HITM = "load hit a Modified line in another core's cache" — the event
  `perf c2c` is built on. `perf c2c` itself needs `perf mem` (PEBS load-latency
  sampling), which WSL2 does not expose; the raw counter
  `mem_load_l3_hit_retired.xsnp_hitm` does work, so that's what's reported.
  Say exactly this if asked "did you use perf c2c".

## 4. MPSC ring — `include/shmring/mpsc_ring.hpp` (Vyukov bounded queue)

- Each cell has a `seq`. `seq == pos`: free for the producer holding ticket
  `pos`. `seq == pos+1`: full, for the consumer at `pos`. Consumer frees it by
  setting `seq = pos + N`.
- Producers CAS the shared `enq` counter to claim a ticket, write the cell,
  publish with `seq.store(pos+1, release)`. Consumer is single, so its index is
  a plain integer — no CAS on the consumer side.
- One cell per cache line so two producers writing neighbouring cells don't
  false-share. Costs memory; deliberate.
- **Known cross-process hole**: a producer killed between winning the CAS and
  publishing `seq` leaves a cell the consumer waits on forever. Fix would be a
  per-producer heartbeat + skip/poison protocol. Not built; say so.
- Throughput falls with more producers (CAS contention on one line) — see
  `results/`.

## 5. Seqlock — `include/shmring/seqlock.hpp`

- Writer: `seq = s+1` (odd), write data, `seq = s+2`. Reader: read seq, copy,
  read seq again; retry if odd or changed. Writer never waits for readers.
- Use it for **latest value** data (top-of-book snapshot); use a ring when
  every event matters.
- **Payload stored as `std::atomic<uint64_t>` words.** A plain `memcpy` racing
  with the writer is a data race = UB even if you discard the result. Relaxed
  atomics are plain MOVs on x86, so it costs nothing, and TSan is clean.
- **No standalone fences**: data stores are `release`, data loads `acquire`.
  That gives both required orderings (writer's odd-seq store can't sink below a
  data store; reader's second seq load can't hoist above a data load) and TSan
  understands it (it does not model `atomic_thread_fence`).
- Consequence found in practice: because the snapshot is *latest*, when the
  strategy lags the feed the risk check sees a **newer** price than the update
  it's processing — decisions become timing-dependent. Seen under Valgrind
  (slow consumer → different OUCH checksum). The parity test therefore uses
  `--no-snapshot`. In-process vs cross-process parity holds when paced.

## 6. Concepts + templates — `include/shmring/concepts.hpp`, `channel.hpp`

- `ShmPayload<T>` = trivially copyable + standard layout → rejects
  `std::string`, `std::vector`, anything virtual, at compile time. It can't
  catch a raw pointer member; that's documented.
- `Queue<Q>` = has `value_type`, `capacity`, `try_push`, `try_pop`.
- `Ring<T, N, Producers::One|Many>` = `std::conditional_t` → `SpscRing` or
  `MpscRing`. Generic code takes `template <Queue Q>`; it's instantiated for
  the concrete ring, so it inlines like hand-written code.
- **Honest benchmark result**: direct, concept-generic and *virtual* all
  measured ~4.5–5 ns per push+pop. A perfectly predicted indirect call is
  nearly free when the body is 5 ns of dependent memory ops. The template's
  real advantage is that the call can be *inlined* and optimised with the
  caller; the virtual version can't. Don't claim "virtual was N× slower".

## 7. Object pool — `include/shmring/object_pool.hpp`

- Storage array reserved at construction; free list is a stack of indices.
- `acquire(args...)` = pop index + **placement new** (`::new (ptr) T(args...)`);
  `release(p)` = `p->~T()` + push index. LIFO → the next object handed out is
  the most recently freed, so it's cache-hot.
- Exhausted → `nullptr`, never a silent heap fallback.
- Single-threaded: one pool per thread/process.

## 8. Allocation proof — two independent tools

1. `alloc_counter.hpp`: replaces global `operator new` in one TU and counts.
   `SHMRING_ALLOC_TRACE=1` prints a backtrace for every steady-state
   allocation — this is how both offenders were found:
   - **risk gate**: 2 allocations per accepted order (a `std::map` node for the
     self-trade price book and an `std::unordered_map` node for the open
     order). Fixed by moving those containers to `std::pmr` on an
     `unsynchronized_pool_resource` — freed nodes are recycled.
   - **book**: price-level `std::vector` growth. Fixed by reserving 64 levels
     per side; 2–3 growths remain over 3M messages for books deeper than 64.
2. **Valgrind memcheck**: run feed + strategy at 1M and 2M messages; startup is
   identical, so `allocs(2M) − allocs(1M)` = allocations caused by messages.
   Needs `--soname-synonyms=somalloc=nouserintercepts` because the binary
   replaces `operator new` itself (without it Valgrind half-intercepts and
   reports "mismatched free/delete").
   Gotcha found: the strategy's attach-retry loop builds a `std::string` per
   retry → timing-dependent startup allocations polluted the delta. Script now
   starts the feed first.

## 9. Measuring latency — `bench/ipc_latency.cpp`, `bench/hiccup.cpp`

- Ping-pong, RTT/2. Same method for every transport, and no cross-core TSC
  comparison needed for this one.
- `rdtsc` (not `rdtscp`): in tick2trade, `rdtscp` serialisation cost tens of
  percent of the path being timed. `lfence; rdtsc` around a ~250 ns round trip
  is fine.
- Blocking pipe/UDS/eventfd include the **scheduler wake-up** (~27 µs here —
  WSL2/Hyper-V inflates wake-ups; bare metal is typically single-digit µs).
  `--spin` (O_NONBLOCK + busy poll) removes the wake-up and leaves the syscall
  + copy cost — still several hundred ns to µs. shm always spins.
- **Jitter floor**: `hiccup` spins on one pinned core and records gaps in its
  own TSC stream. Under WSL2 each vCPU disappears ~2,000 times/s
  (median ~10 µs, p99 ~115–150 µs, max 10–14 ms) — Hyper-V descheduling the vCPU.
  Every tail on this machine sits on top of that.

## 10. The tick2trade split — `apps/t2t_core.hpp`, `apps/t2t_split.cpp`

```
feed process (cpu 1)                        strategy process (cpu 3)
mmap ITCH file                              attach ring + snapshot table
decode + book (tick2trade)                  pop TobUpdate (64 B = 1 line)
seqlock snapshot[sym] = BBO   ───shm───▶    signal (imbalance, per symbol)
push TobUpdate{t_rx, t_push, BBO}           ObjectPool<Order>.acquire
                                            risk Gate.submit (algo-risk-gateway)
                                            OUCH encode, t_out = rdtsc
```

- Tick-to-order = `t_out − t_rx`, both from the same invariant TSC.
  Decomposed into feed / ring hop / strategy stages.
- **Pacing** (`--gap-ns 2000`): book events 2 µs apart. Unpaced, the feed
  outruns the strategy and you measure queueing, not latency.
- **In-process baseline**: same classes, direct call, no ring → the difference
  is the process boundary.
- **Why the cross-process tail is so much worse than in-process**: one vCPU
  stall of the strategy process queues *every* event behind it (500k
  events/s); in-process, a stall delays only the event in hand. The hop median
  is stable (~106–110 ns); the tail is the platform. Don't quote xproc p99 as
  a property of the ring.
- **Parity**: inproc and xproc emit byte-identical OUCH (same checksum) — the
  split changes nothing about behaviour. ctest + CI.

## 11. Questions to expect

- *Why not `std::atomic<T>` for the whole slot?* Not lock-free for 64 B; the
  head/tail protocol already orders the plain slot write.
- *Why can't the consumer read the slot with relaxed head?* Then nothing
  orders the slot read after the producer's slot write (canary demonstrates).
- *What if the consumer process dies?* Producer fills the ring and `try_push`
  returns false; detection needs a heartbeat (not built). Restart: creator
  unlinks and recreates; attachers validate via the header.
- *Why `alignas(64)` and not `hardware_destructive_interference_size`?* GCC
  warns its value can change with flags; two processes must agree on layout.
  (Adjacent-line prefetch on Intel can make 128 B better — not measured here.)
- *What does `MAP_POPULATE` buy?* No page faults on first touch; also
  `mlock` would stop the pages being swapped (not used).
- *Why is MPSC slower?* CAS on one shared counter + one line per cell.
- *Is the ring wait-free?* `try_push`/`try_pop` are wait-free for SPSC (bounded
  steps); MPSC `try_push` is lock-free (CAS retry loop), not wait-free.
