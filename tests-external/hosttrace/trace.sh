#!/bin/bash
# Usage: trace.sh <test> [out]
# Builds gemmini-rocc-tests/bareMetalC/<test>.c for the host and writes its Gemmini
# instruction trace (with the DRAM bytes each mvin reads) to out (default: stdout).
set -e
TEST=$1
D=$(cd "$(dirname "$0")" && pwd)
T=$D/../dependencies/gemmini-rocc-tests
BIN=$(mktemp)
trap 'rm -f "$BIN"' EXIT
gcc -w -O0 -std=gnu99 -DBAREMETAL=1 -DPREALLOCATE=1 -I"$D" -I"$T" -I"$T/riscv-tests" -I"$T/riscv-tests/env" \
  "$T/bareMetalC/$TEST.c" -o "$BIN" -lm
"$BIN" 2>"${2:-/dev/stdout}" >/dev/null
