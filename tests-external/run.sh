#!/bin/bash
# Usage: [SEEDS=20] [OPS=<gen_random.py --ops>] [JOBS=<CPU count>] run.sh [ila-dir]
#   ila-dir: ILA version to test, a directory with src/gemmini.cc and include/gemmini.h,
#            relative to the Gemmini-ila root (default: . ; e.g. new)
# Builds gemmini_diff in build/ against ila-dir, then runs it, JOBS programs at a time, on
#   1. the gemmini-rocc-tests programs that only use instructions the ILA can encode
#      (funct 0-7), traced on the host, and
#   2. SEEDS constrained-random programs from gen_random.py.
# Prints one line per program as it finishes; full reports go to build/results/<name>.out
# and per-instruction progress to logs/<name>.log.

# Treat unset variables as errors.
set -u
# D: absolute path of this script's directory (tests-external).
D=$(cd "$(dirname "$0")" && pwd)
# ILA: absolute path of the ILA version to test (first argument, relative to the Gemmini-ila root).
ILA=$(cd "$D/.." && cd "${1:-.}" && pwd) || exit 1
[ -f "$ILA/src/gemmini.cc" ] || { echo "no src/gemmini.cc in $ILA"; exit 1; }
# DIM: systolic array size; fixed at 16, the size libgemmini and the rocc-tests are written for.
DIM=16
# B: absolute path of the build directory, shared by every ILA version.
B=$D/build
mkdir -p "$B"

# Configure the build for the chosen ILA version, then rebuild gemmini_diff; the output
# goes to build.log, which is shown only when the build fails.
echo "Building gemmini_diff against $ILA"
if ! { cmake -S "$D" -B "$B" -DCMAKE_BUILD_TYPE=Release -DGEMMINI_ILA_DIR="$ILA" &&
       cmake --build "$B" --target gemmini_diff -j; } > "$B/build.log" 2>&1; then
  tail -n 30 "$B/build.log"
  echo "build failed, full log: $B/build.log"
  exit 1
fi

# LOGS: where gemmini_diff writes per-instruction progress, one file per program.
LOGS=$D/logs
# gemmini-rocc-tests programs (bareMetalC/<name>.c) that only use ILA-encodable instructions.
ROCC_TESTS="aligned global_average matmul matmul_os matmul_ws matrix_add mvin_mvout mvin_mvout_acc
  mvin_mvout_acc_full mvin_mvout_acc_full_stride mvin_mvout_acc_stride mvin_mvout_acc_zero_stride
  mvin_mvout_block_stride mvin_mvout_stride mvin_mvout_zeros mvin_scale padded raw_hazard template
  tiled_matmul_os transpose"
# Create the output directories for traces, reports and progress logs.
mkdir -p "$B/traces" "$B/results" "$LOGS"

# Writes the instruction trace of program <name> to file <trace>.
write_trace() { # write_trace <name> <trace>
  case $1 in
    # random-<seed>: generate a random program; ${1#random-} strips the prefix to get the seed,
    # and --ops is passed only when OPS is non-empty.
    random-*) "$D/gen_random.py" --dim "$DIM" --seed "${1#random-}" ${OPS:+--ops "$OPS"} > "$2" ;;
    # Anything else is a gemmini-rocc-tests program: compile it for the host and record its trace.
    *) "$D/hosttrace/trace.sh" "$1" "$2" ;;
  esac
}

# Traces program <name>, replays it through gemmini_diff, and prints one PASS/FAIL/SKIP line.
run_test() { # run_test <name>
  # Per-program trace file and full gemmini_diff report.
  local trace=$B/traces/$1.trace out=$B/results/$1.out
  # A program that cannot be traced is skipped rather than counted as a failure.
  write_trace "$1" "$trace" || { echo "SKIP $1: cannot write trace"; return 0; }
  # Compare the ILA against libgemmini instruction by instruction; the exit status decides the verdict.
  if "$B/gemmini_diff" --progress "$LOGS/$1.log" "$trace" > "$out" 2>&1; then
    echo "PASS $1"
  else
    # Summarise the failure from the report: line 2 is the mismatching trace line (whitespace
    # squeezed), line 4 is the first state difference.
    echo "FAIL $1: $(sed -nE '2{s/^ *//;s/ +/ /g;p;}' "$out") -> $(sed -n '4{s/^ *//;p;}' "$out")"
  fi
}
# Make the functions and variables visible to the child bash processes that xargs starts.
export -f write_trace run_test
export D B DIM LOGS
export OPS=${OPS:-}

# Build the list of programs to run: the rocc-tests, then random-1 .. random-$SEEDS (default 20).
tests=$ROCC_TESTS
for s in $(seq 1 "${SEEDS:-20}"); do tests="$tests random-$s"; done

echo "Logging per-instruction progress to $LOGS/<name>.log"
# Every result line is also saved here so it can be counted at the end.
summary=$B/results/summary.txt
# One program name per line into xargs, which runs up to JOBS (default: CPU count) run_test calls
# in parallel, each in a new bash; '_' fills $0 so the name lands in $1. tee prints each line
# live and saves it to the summary.
printf '%s\n' $tests | xargs -P "${JOBS:-$(getconf _NPROCESSORS_ONLN)}" -I{} \
  "$BASH" -c 'run_test "$1"' _ {} | tee "$summary"

# Count the results and report totals.
pass=$(grep -c '^PASS' "$summary")
fail=$(grep -c '^FAIL' "$summary")
echo "$pass passed, $fail failed"
# Exit status: 0 only when nothing failed (SKIPs do not fail the run).
[ "$fail" = 0 ]
