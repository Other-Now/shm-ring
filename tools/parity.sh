#!/usr/bin/env bash
# The split must not change what the system does: the in-process run and the
# two-process run over the same feed must emit byte-identical OUCH (same
# checksum, same order count). Used by ctest and CI.
#
# --no-snapshot: the seqlock snapshot is "latest value", so when the strategy
# lags the feed the risk check sees a newer price than the update it is
# handling and decisions become timing-dependent (seen under Valgrind). Parity
# is a determinism check, so it takes the reference price from the update.
set -euo pipefail
T2T=${T2T:-./t2t_split}
FILE=${1:?usage: parity.sh <itch-file> [extra t2t_split args]}
shift || true

in=$("$T2T" inproc "$FILE" --warmup 0 --no-snapshot "$@" 2>/dev/null)
xp=$("$T2T" run "$FILE" --warmup 0 --no-snapshot "$@" 2>/dev/null | grep '^xproc')

# columns: mode,msgs,updates,orders,p50,p99,p999,max,mean,checksum,allocs
in_key=$(echo "$in" | awk -F, '{print $3","$4","$10}')
xp_key=$(echo "$xp" | awk -F, '{print $3","$4","$10}')
echo "inproc updates,orders,checksum = $in_key"
echo "xproc  updates,orders,checksum = $xp_key"
if [[ "$in_key" != "$xp_key" || -z "$in_key" ]]; then
  echo "PARITY FAIL"
  exit 1
fi
echo "PARITY OK"
