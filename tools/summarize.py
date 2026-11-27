#!/usr/bin/env python3
"""Turn results/raw/<tag>/ into markdown tables (medians across repetitions)."""
import csv
import json
import os
import statistics as st
import sys
from collections import defaultdict

D = sys.argv[1]


def rows(name):
    p = os.path.join(D, name)
    if not os.path.exists(p):
        return []
    with open(p) as f:
        return [r for r in csv.reader(f) if r]


def med(xs):
    return st.median(xs) if xs else float("nan")


def rng(xs):
    return f"{min(xs):.0f}–{max(xs):.0f}" if xs else ""


out = []
P = out.append

# ---------------------------------------------------------------- jitter
h = rows("hiccup.csv")
if h:
    P("## Platform jitter floor (`hiccup`, spin on one pinned core)\n")
    P("| cpu | runs | gaps > 1 µs per s | gap p50 µs | gap p99 µs | max gap µs | time lost % |")
    P("|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for r in h:
        by[r[0]].append(r)
    for cpu, rs in sorted(by.items()):
        P(f"| {cpu} | {len(rs)} | {med([float(r[2]) / float(r[1]) for r in rs]):.0f} | "
          f"{med([float(r[3]) for r in rs]):.1f} | {med([float(r[4]) for r in rs]):.0f} | "
          f"{max(float(r[5]) for r in rs):.0f} | {med([float(r[6]) for r in rs]):.2f} |")
    P("")

# ---------------------------------------------------------------- latency
lat = rows("ipc_latency.csv")
if lat:
    P("## Cross-process one-way latency (ping-pong RTT/2, ns, median of runs)\n")
    P("| transport | cpus | runs | p50 | p99 | p99.9 | p50 range |")
    P("|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for r in lat:
        by[(r[0], f"{r[1]},{r[2]}")].append(r)
    order = ["shm", "shm-huge", "eventfd-spin", "pipe-spin", "uds-spin", "eventfd-block", "pipe-block", "uds-block"]
    for (name, cpus), rs in sorted(by.items(), key=lambda kv: (kv[0][1], order.index(kv[0][0]) if kv[0][0] in order else 99)):
        p50 = [float(r[5]) for r in rs]
        P(f"| {name} | {cpus} | {len(rs)} | {med(p50):,.0f} | {med([float(r[6]) for r in rs]):,.0f} | "
          f"{med([float(r[7]) for r in rs]):,.0f} | {rng(p50)} |")
    P("")

# ---------------------------------------------------------------- false sharing
fs = rows("false_sharing.csv")
if fs:
    P("## SPSC layout 2×2 (two processes, 20M × 8-byte msgs, perf stat over both)\n")
    P("| layout | runs | M msg/s (median) | range | ns/msg | HITM per 1k msgs | memory-ordering clears per 1k msgs | IPC |")
    P("|---|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for r in fs:
        ctr = {}
        for kv in ",".join(r[6:]).split(";"):
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    ctr[k.split(":")[0]] = float(v)
                except ValueError:
                    pass
        by[r[0]].append((r, ctr))
    for name in ["spsc-packed", "spsc-padded", "spsc-packed-cached", "spsc-cached"]:
        rs = by.get(name)
        if not rs:
            continue
        n = float(rs[0][0][2])
        mps = [float(r[3]) for r, _ in rs]
        hitm = [c.get("mem_load_l3_hit_retired.xsnp_hitm", float("nan")) / n * 1000 for _, c in rs]
        mc = [c.get("machine_clears.memory_ordering", float("nan")) / n * 1000 for _, c in rs]
        ipc = [c.get("instructions", 0) / c.get("cycles", 1) for _, c in rs]
        P(f"| {name} | {len(rs)} | {med(mps):.1f} | {min(mps):.1f}–{max(mps):.1f} | {1000 / med(mps):.1f} | "
          f"{med(hitm):.1f} | {med(mc):.1f} | {med(ipc):.2f} |")
    P("")

mp = rows("mpsc.csv")
if mp:
    P("## MPSC ring, P producer processes → 1 consumer\n")
    P("| producers | runs | M msg/s (median) | range | order errors |")
    P("|---|---|---|---|---|")
    by = defaultdict(list)
    for r in mp:
        by[r[1]].append(r)
    for p, rs in sorted(by.items()):
        v = [float(r[3]) for r in rs]
        P(f"| {p} | {len(rs)} | {med(v):.1f} | {min(v):.1f}–{max(v):.1f} | {sum(int(r[5]) for r in rs)} |")
    P("")

# ---------------------------------------------------------------- t2t
t = rows("t2t.csv")
if t:
    P("## tick2trade split, 3M real ITCH messages, book events paced 2 µs apart (ns, median of runs)\n")
    P("| reference price | mode | runs | orders | tick-to-order p50 | p99 | p99.9 | feed p50 | ring hop p50 | strategy+risk+encode p50 | transit p50 (all updates) | transit p99 | checksums agree |")
    P("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for r in t:
        by[(r[0], r[1])].append(r)
    for snap in ["snap", "nosnap"]:
        for mode in ["inproc", "xproc"]:
            rs = by.get((snap, mode))
            if not rs:
                continue
            f = lambda i: med([float(r[i]) for r in rs])
            sums = {r[10] for r in by.get((snap, "inproc"), []) + by.get((snap, "xproc"), [])}
            P(f"| {'seqlock snapshot' if snap == 'snap' else 'from update'} | {mode} | {len(rs)} | {rs[0][4]} | "
              f"{f(5):,.0f} | {f(6):,.0f} | {f(7):,.0f} | {f(12):,.0f} | {f(13):,.0f} | {f(15):,.0f} | "
              f"{f(16):,.0f} | {f(17):,.0f} | {'yes' if len(sums) == 1 else 'no (' + str(len(sums)) + ')'} |")
    P("")

# ---------------------------------------------------------------- micro
mj = os.path.join(D, "micro.json")
if os.path.exists(mj):
    with open(mj) as f:
        j = json.load(f)
    P("## Google Benchmark (single core, median of 5 repetitions)\n")
    P("| benchmark | ns | items/s |")
    P("|---|---|---|")
    for b in j["benchmarks"]:
        if b.get("aggregate_name") != "median":
            continue
        ips = b.get("items_per_second")
        P(f"| {b['run_name']} | {b['real_time']:.2f} | {'' if not ips else f'{ips / 1e6:.0f} M'} |")
    P("")

# ---------------------------------------------------------------- allocations
al = os.path.join(D, "allocations.txt")
if os.path.exists(al):
    P("## Allocations\n")
    P("```")
    P(open(al).read().rstrip())
    P("```")

print("\n".join(out))
