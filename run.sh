#!/bin/bash
# Usage: ./run.sh <suite> [new]
#   suite:  tests           unit tests that prove properties of each ILA instruction
#           tests-external  differential tests against libgemmini (see tests-external/README.md)
#   new:    test the ILA in new/ instead of the one in src/ and include/
# Both suites share build/, so the dependencies are compiled only once.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
SUITE=${1:-}
VERSION=${2:-}

usage() {
  sed -n '2,6s/^# \{0,1\}//p' "$0"
  exit 1
}

case $SUITE in
  tests)          TARGET=test_gemmini_ila ;;
  tests-external) TARGET=gemmini_diff ;;
  *)              usage ;;
esac

case $VERSION in
  "")  ILA_DIR=$ROOT ;;
  new) ILA_DIR=$ROOT/new ;;
  *)   usage ;;
esac

echo "Building $TARGET against $ILA_DIR"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DGEMMINI_ILA_DIR="$ILA_DIR"
cmake --build "$ROOT/build" --target "$TARGET" -j

case $SUITE in
  tests)          "$ROOT/build/tests/test_gemmini_ila" ;;
  tests-external) "$ROOT/tests-external/run_programs.sh" ;;
esac
