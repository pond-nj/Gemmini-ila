#!/bin/bash
# Usage: run.sh [build-dir]        (default: build; env SEEDS=20, OPS=<gen_random.py --ops>,
#                                   JOBS=<CPU count>)
# Runs gemmini_diff, JOBS programs at a time, on
#   1. the gemmini-rocc-tests programs that only use instructions the ILA can encode
#      (funct 0-7), traced on the host (they are written for DIM 16), and
#   2. SEEDS constrained-random programs from gen_random.py.
# Prints one line per program as it finishes; full reports go to <build-dir>/results/<name>.out
# and per-instruction progress to logs/<name>.log.
set -u
D=$(cd "$(dirname "$0")" && pwd)
B=$(cd "${1:-$D/build}" && pwd)
DIM=$(sed -n 's/^GEMMINI_DIM:STRING=//p' "$B/CMakeCache.txt")
LOGS=$D/logs
ROCC_TESTS="aligned global_average matmul matmul_os matmul_ws matrix_add mvin_mvout mvin_mvout_acc
  mvin_mvout_acc_full mvin_mvout_acc_full_stride mvin_mvout_acc_stride mvin_mvout_acc_zero_stride
  mvin_mvout_block_stride mvin_mvout_stride mvin_mvout_zeros mvin_scale padded raw_hazard template
  tiled_matmul_os transpose"
mkdir -p "$B/traces" "$B/results" "$LOGS"

write_trace() { # write_trace <name> <trace>
  case $1 in
    random-*) "$D/gen_random.py" --dim "$DIM" --seed "${1#random-}" ${OPS:+--ops "$OPS"} > "$2" ;;
    *) "$D/hosttrace/trace.sh" "$1" "$2" ;;
  esac
}

run_test() { # run_test <name>
  local trace=$B/traces/$1.trace out=$B/results/$1.out
  write_trace "$1" "$trace" || { echo "SKIP $1: cannot write trace"; return 0; }
  if "$B/gemmini_diff" --progress "$LOGS/$1.log" "$trace" > "$out" 2>&1; then
    echo "PASS $1"
  else
    echo "FAIL $1: $(sed -n '2{s/^ *//;s/ \+/ /g;p}' "$out") -> $(sed -n '4{s/^ *//;p}' "$out")"
  fi
}
export -f write_trace run_test
export D B DIM LOGS
export OPS=${OPS:-}

tests=""
if [ "$DIM" = 16 ]; then
  tests=$ROCC_TESTS
else
  echo "skipping gemmini-rocc-tests: they need GEMMINI_DIM=16, this build has $DIM"
fi
for s in $(seq 1 "${SEEDS:-20}"); do tests="$tests random-$s"; done

echo "Logging per-instruction progress to $LOGS/<name>.log"
summary=$B/results/summary.txt
printf '%s\n' $tests | xargs -P "${JOBS:-$(getconf _NPROCESSORS_ONLN)}" -I{} \
  "$BASH" -c 'run_test "$1"' _ {} | tee "$summary"

pass=$(grep -c '^PASS' "$summary")
fail=$(grep -c '^FAIL' "$summary")
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
