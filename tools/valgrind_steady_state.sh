#!/usr/bin/env bash
# Steady-state allocations, counted by Valgrind (sees every malloc, including
# ones the operator-new counter cannot: C library, direct malloc).
#
# Runs feed + strategy as two processes, each under memcheck, at two feed
# lengths N1 < N2. Startup allocations are identical in both runs, so
#   allocs(N2) - allocs(N1)  =  allocations caused by messages N1..N2.
# Also reports memcheck's error count (invalid reads/writes etc.).
#
#   tools/valgrind_steady_state.sh <t2t_split binary> <itch-file> N1 N2 [outdir]
set -euo pipefail
BIN=${1:?binary}; FILE=${2:?itch file}; N1=${3:?N1}; N2=${4:?N2}; OUT=${5:-/tmp/sr-vg}
# nouserintercepts: the binary replaces operator new/delete (alloc counter);
# let Valgrind count the malloc/free underneath instead of half-intercepting.
VG="${VALGRIND:-valgrind} --soname-synonyms=somalloc=nouserintercepts"
mkdir -p "$OUT"

allocs() { grep -oP 'total heap usage: \K[0-9,]+(?= allocs)' "$1" | tr -d ,; }
errors() { grep -oP 'ERROR SUMMARY: \K[0-9]+' "$1"; }

for N in "$N1" "$N2"; do
  rm -f /dev/shm/shmring_t2t_*
  # Feed first, strategy only once the segments exist: otherwise the strategy's
  # attach-poll loop (which builds a std::string per retry) adds a
  # timing-dependent number of startup allocations and pollutes the delta.
  $VG --log-file="$OUT/feed_$N.txt" "$BIN" feed "$FILE" --max "$N" --warmup 0 >"$OUT/feed_$N.out" 2>&1 &
  FP=$!
  while [ ! -e /dev/shm/shmring_t2t_ctl ]; do sleep 0.05; done
  sleep 0.5
  $VG --log-file="$OUT/strategy_$N.txt" "$BIN" strategy --warmup 0 >"$OUT/strategy_$N.out" 2>&1
  wait $FP
done

f1=$(allocs "$OUT/feed_$N1.txt"); f2=$(allocs "$OUT/feed_$N2.txt")
s1=$(allocs "$OUT/strategy_$N1.txt"); s2=$(allocs "$OUT/strategy_$N2.txt")
echo "binary: $(basename "$BIN")   messages $N1 -> $N2"
printf '  feed      allocs %10s -> %10s   delta %6d   memcheck errors %s / %s\n' "$f1" "$f2" $((f2 - f1)) \
  "$(errors "$OUT/feed_$N1.txt")" "$(errors "$OUT/feed_$N2.txt")"
printf '  strategy  allocs %10s -> %10s   delta %6d   memcheck errors %s / %s\n' "$s1" "$s2" $((s2 - s1)) \
  "$(errors "$OUT/strategy_$N1.txt")" "$(errors "$OUT/strategy_$N2.txt")"
