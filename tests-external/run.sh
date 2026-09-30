#!/bin/bash
# Usage: run.sh [build-dir]        (default: build; env SEEDS=20, OPS=<gen_random.py --ops>)
# Runs gemmini_diff on
#   1. the gemmini-rocc-tests programs that only use instructions the ILA can encode
#      (funct 0-7), traced on the host (they are written for DIM 16), and
#   2. SEEDS constrained-random programs from gen_random.py.
# Prints one line per program; full reports go to <build-dir>/results/<name>.out.
set -u
D=$(cd "$(dirname "$0")" && pwd)
B=$(cd "${1:-$D/build}" && pwd)
DIM=$(sed -n 's/^GEMMINI_DIM:STRING=//p' "$B/CMakeCache.txt")
ROCC_TESTS="aligned global_average matmul matmul_os matmul_ws matrix_add mvin_mvout mvin_mvout_acc
  mvin_mvout_acc_full mvin_mvout_acc_full_stride mvin_mvout_acc_stride mvin_mvout_acc_zero_stride
  mvin_mvout_block_stride mvin_mvout_stride mvin_mvout_zeros mvin_scale padded raw_hazard template
  tiled_matmul_os transpose"
mkdir -p "$B/traces" "$B/results"
pass=0 fail=0

check() { # check <name> <trace>
  if "$B/gemmini_diff" "$2" > "$B/results/$1.out" 2>&1; then
    pass=$((pass + 1)); echo "PASS $1"
  else
    fail=$((fail + 1))
    echo "FAIL $1: $(sed -n '2{s/^ *//;s/ \+/ /g;p}' "$B/results/$1.out") -> $(sed -n '4{s/^ *//;p}' "$B/results/$1.out")"
  fi
}

if [ "$DIM" = 16 ]; then
  for t in $ROCC_TESTS; do
    "$D/hosttrace/trace.sh" "$t" "$B/traces/$t.trace" && check "$t" "$B/traces/$t.trace"
  done
else
  echo "skipping gemmini-rocc-tests: they need GEMMINI_DIM=16, this build has $DIM"
fi

for s in $(seq 1 "${SEEDS:-20}"); do
  "$D/gen_random.py" --dim "$DIM" --seed "$s" ${OPS:+--ops "$OPS"} > "$B/traces/random-$s.trace"
  check "random-$s" "$B/traces/random-$s.trace"
done

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
