## Platform jitter floor (`hiccup`, spin on one pinned core)

| cpu | runs | gaps > 1 µs per s | gap p50 µs | gap p99 µs | max gap µs | time lost % |
|---|---|---|---|---|---|---|
| 1 | 3 | 2193 | 10.4 | 148 | 14407 | 5.23 |
| 3 | 3 | 2001 | 10.8 | 115 | 9998 | 3.68 |

## Cross-process one-way latency (ping-pong RTT/2, ns, median of runs)

| transport | cpus | runs | p50 | p99 | p99.9 | p50 range |
|---|---|---|---|---|---|---|
| shm | 1,3 | 3 | 72 | 96 | 124 | 70–74 |
| eventfd-spin | 1,3 | 3 | 1,113 | 2,537 | 12,547 | 1057–1192 |
| pipe-spin | 1,3 | 3 | 1,236 | 2,531 | 12,090 | 1172–1310 |
| uds-spin | 1,3 | 3 | 2,120 | 5,578 | 16,799 | 1986–2236 |
| eventfd-block | 1,3 | 3 | 27,339 | 55,690 | 133,453 | 26717–28666 |
| pipe-block | 1,3 | 3 | 27,736 | 53,455 | 145,194 | 27217–27937 |
| uds-block | 1,3 | 3 | 27,572 | 54,191 | 149,296 | 26481–27588 |
| shm | 2,3 | 3 | 76 | 102 | 118 | 73–77 |
| eventfd-spin | 2,3 | 3 | 1,095 | 2,615 | 16,369 | 1049–1202 |
| pipe-spin | 2,3 | 3 | 1,207 | 2,559 | 12,769 | 1197–1302 |
| uds-spin | 2,3 | 3 | 2,073 | 5,747 | 17,880 | 2065–2703 |
| eventfd-block | 2,3 | 3 | 28,557 | 53,391 | 164,105 | 26296–29145 |
| pipe-block | 2,3 | 3 | 28,386 | 50,596 | 136,425 | 28055–28513 |
| uds-block | 2,3 | 3 | 26,003 | 55,319 | 159,031 | 25326–26778 |

## SPSC layout 2×2 (two processes, 20M × 8-byte msgs, perf stat over both)

| layout | runs | M msg/s (median) | range | ns/msg | HITM per 1k msgs | memory-ordering clears per 1k msgs | IPC |
|---|---|---|---|---|---|---|---|
| spsc-packed | 7 | 78.1 | 59.6–86.3 | 12.8 | 64.4 | 407.6 | 0.48 |
| spsc-padded | 7 | 70.1 | 62.4–103.2 | 14.3 | 376.1 | 420.9 | 0.46 |
| spsc-packed-cached | 7 | 66.7 | 57.2–73.0 | 15.0 | 94.7 | 432.5 | 0.39 |
| spsc-cached | 7 | 151.1 | 139.8–175.5 | 6.6 | 136.2 | 98.2 | 0.97 |

## MPSC ring, P producer processes → 1 consumer

| producers | runs | M msg/s (median) | range | order errors |
|---|---|---|---|---|
| 1 | 3 | 46.3 | 43.6–47.2 | 0 |
| 2 | 3 | 24.8 | 19.9–31.5 | 0 |
| 3 | 3 | 16.6 | 15.3–17.1 | 0 |

## tick2trade split, 3M real ITCH messages, book events paced 2 µs apart (ns, median of runs)

| reference price | mode | runs | orders | tick-to-order p50 | p99 | p99.9 | feed p50 | ring hop p50 | strategy+risk+encode p50 | transit p50 (all updates) | transit p99 | checksums agree |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| seqlock snapshot | inproc | 5 | 12860 | 347 | 1,179 | 3,079 | 119 | 0 | 198 | 0 | 0 | yes |
| seqlock snapshot | xproc | 5 | 12860 | 569 | 7,551 | 127,319 | 124 | 106 | 288 | 125 | 5,661 | yes |
| from update | inproc | 5 | 12860 | 339 | 1,316 | 5,902 | 118 | 0 | 193 | 0 | 0 | yes |
| from update | xproc | 5 | 12860 | 605 | 11,722 | 268,056 | 133 | 110 | 305 | 115 | 10,242 | yes |

## Google Benchmark (single core, median of 5 repetitions)

| benchmark | ns | items/s |
|---|---|---|
| spsc_push_pop/packed | 5.91 |  |
| spsc_push_pop/padded | 5.86 |  |
| spsc_push_pop/padded_cached | 5.63 |  |
| mpsc_push_pop | 10.21 |  |
| spsc_burst64/padded | 208.17 | 320 M |
| spsc_burst64/padded_cached | 198.37 | 336 M |
| mpsc_burst64 | 758.29 | 85 M |
| seqlock_store | 1.37 |  |
| seqlock_load | 4.81 |  |
| pool_acquire_release | 7.42 |  |
| new_delete | 15.94 |  |
| pool_churn256 | 2.30 |  |
| heap_churn256 | 16.29 |  |
| abstraction/direct | 3.10 |  |
| abstraction/template_concept | 3.08 |  |
| abstraction/virtual | 3.26 |  |

## Allocations

```
== operator-new counter, 3M messages, 200k warm-up
-- t2t_split_heapgate --no-reserve
[feed] messages 3000000  book events 2779724  unknown refs 0  steady-state operator-new calls: 4389
[xproc] OUCH checksum 5e6615dd2a6b7a79   steady-state operator-new calls (strategy side): 25716
-- t2t_split_heapgate 
[feed] messages 3000000  book events 2779724  unknown refs 0  steady-state operator-new calls: 3
[xproc] OUCH checksum 5e6615dd2a6b7a79   steady-state operator-new calls (strategy side): 25716
-- t2t_split --no-reserve
[feed] messages 3000000  book events 2779724  unknown refs 0  steady-state operator-new calls: 4389
[xproc] OUCH checksum 5e6615dd2a6b7a79   steady-state operator-new calls (strategy side): 2
-- t2t_split 
[feed] messages 3000000  book events 2779724  unknown refs 0  steady-state operator-new calls: 3
[xproc] OUCH checksum de4bcb7d9aadbaad   steady-state operator-new calls (strategy side): 2
== valgrind memcheck, delta between 1M and 2M messages
binary: t2t_split_heapgate   messages 1000000 -> 2000000
  feed      allocs      32786 ->      32788   delta      2   memcheck errors 0 / 0
  strategy  allocs       7007 ->      16941   delta   9934   memcheck errors 0 / 0
binary: t2t_split   messages 1000000 -> 2000000
  feed      allocs      32786 ->      32788   delta      2   memcheck errors 0 / 0
  strategy  allocs         22 ->         22   delta      0   memcheck errors 0 / 0
```
