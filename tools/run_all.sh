#!/usr/bin/env bash
# The full measurement campaign behind results/*.md. Writes raw CSV/text to
# results/raw/<tag>/ and then calls tools/summarize.py.
#
#   tools/run_all.sh <build-dir> <itch-file> [tag]
#
# Env: PERF (perf binary, default "perf"), VALGRIND (valgrind command, default
# "valgrind"), REPS (default 5), CROSS="1,3" and SMT="2,3" (cpu pairs: two
# different physical cores / two hyperthreads of one core; check lscpu -e).
set -euo pipefail
B=$(cd "${1:?build dir}" && pwd); FILE=${2:?itch file}; TAG=${3:-local}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$HERE/results/raw/$TAG; mkdir -p "$OUT"
PERF=${PERF:-perf}; VALGRIND=${VALGRIND:-valgrind}; REPS=${REPS:-5}
CROSS=${CROSS:-1,3}; SMT=${SMT:-2,3}
log() { echo "[$(date +%T)] $*" >&2; }

{ uname -a; lscpu | grep -E "Model name|^CPU\(s\)|Thread|Core|Socket|L1d|L2|L3"; lscpu -e; } > "$OUT/machine.txt" 2>&1 || true

# 1. jitter floor ----------------------------------------------------------
log "hiccup"
: > "$OUT/hiccup.csv"
for r in $(seq 3); do for c in ${CROSS//,/ }; do "$B/hiccup" --cpu "$c" --secs 5 >> "$OUT/hiccup.csv"; done; done

# 2. ping-pong latency -----------------------------------------------------
log "ipc_latency"
: > "$OUT/ipc_latency.csv"
for r in $(seq 3); do
  for pair in "$CROSS" "$SMT"; do
    "$B/ipc_latency" --transport shm --cpus "$pair" --iters 1000000 >> "$OUT/ipc_latency.csv"
    for t in pipe uds eventfd; do
      "$B/ipc_latency" --transport $t --spin --cpus "$pair" --iters 300000 --warmup 30000 >> "$OUT/ipc_latency.csv"
      "$B/ipc_latency" --transport $t --cpus "$pair" --iters 100000 --warmup 10000 >> "$OUT/ipc_latency.csv"
    done
  done
done
if [ "$(cat /proc/sys/vm/nr_hugepages)" -gt 0 ]; then
  for r in $(seq 3); do "$B/ipc_latency" --transport shm --huge --cpus "$CROSS" --iters 1000000 >> "$OUT/ipc_latency.csv"; done
fi

# 3. false sharing 2x2 under perf stat ---------------------------------------
log "ring_throughput"
EVENTS=mem_load_l3_hit_retired.xsnp_hitm,machine_clears.memory_ordering,instructions,cycles
"$PERF" stat -e mem_load_l3_hit_retired.xsnp_hitm true >/dev/null 2>&1 || EVENTS=instructions,cycles
: > "$OUT/false_sharing.csv"
for r in $(seq 7); do
  for l in packed padded packed-cached cached; do
    res=$("$PERF" stat -x, -e "$EVENTS" "$B/ring_throughput" --layout $l --msgs 20000000 --cpus "$CROSS" 2>"$OUT/perf.tmp")
    ctr=$(grep -v '^spsc' "$OUT/perf.tmp" | awk -F, 'NF>3{printf "%s=%s;", $3, $1}')
    echo "$res,$ctr" >> "$OUT/false_sharing.csv"
  done
done
rm -f "$OUT/perf.tmp"
: > "$OUT/mpsc.csv"
for r in $(seq 3); do for p in 1 2 3; do "$B/ring_throughput" --mpsc $p --msgs 5000000 >> "$OUT/mpsc.csv"; done; done

# 4. tick2trade split --------------------------------------------------------
log "t2t_split"
: > "$OUT/t2t.csv"
for r in $(seq "$REPS"); do
  for snap in "" "--no-snapshot"; do
    tag=$([ -z "$snap" ] && echo snap || echo nosnap)
    "$B/t2t_split" inproc "$FILE" --max 3000000 --gap-ns 2000 $snap 2>/dev/null | sed "s/^/$tag,/" >> "$OUT/t2t.csv"
    "$B/t2t_split" run "$FILE" --max 3000000 --gap-ns 2000 $snap 2>/dev/null | grep '^xproc' | sed "s/^/$tag,/" >> "$OUT/t2t.csv"
  done
done
"$B/t2t_split" run "$FILE" --max 3000000 --gap-ns 2000 > /dev/null 2> "$OUT/t2t_example.txt"

# 5. allocations: operator-new counter and Valgrind, before/after --------------
log "allocations"
{
  echo "== operator-new counter, 3M messages, 200k warm-up"
  for b in t2t_split_heapgate t2t_split; do
    for res in "--no-reserve" ""; do
      echo "-- $b $res"
      "$B/$b" run "$FILE" --max 3000000 --gap-ns 0 $res 2>&1 >/dev/null | grep operator-new
    done
  done
  echo "== valgrind memcheck, delta between 1M and 2M messages"
  for b in t2t_split_heapgate t2t_split; do
    VALGRIND="$VALGRIND" bash "$HERE/tools/valgrind_steady_state.sh" "$B/$b" "$FILE" 1000000 2000000 "$OUT/vg-$b"
  done
} > "$OUT/allocations.txt" 2>&1

# 6. microbenchmarks ----------------------------------------------------------
log "micro"
"$B/micro" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_min_time=0.3s \
  --benchmark_out="$OUT/micro.json" --benchmark_out_format=json > "$OUT/micro.txt" 2>&1

log "summarize"
python3 "$HERE/tools/summarize.py" "$OUT" > "$OUT/summary.md"
log "done -> $OUT/summary.md"
