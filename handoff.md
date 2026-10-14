# Handoff: directed traces for the old ILA's discrepancies

## Where things stand

- `tests-external/directed/` has 8 hand-written traces, one per bug in
  `tests-external/test.md` (bugs 1-8). Each one fails on the old ILA (`src/`, `include/`)
  at the targeted instruction and passes on `new/`. Each trace's header comments give:
  the bug, `# Where:` (the old ILA line that is wrong or missing), `# Expected:`, and what
  the old ILA does.
- `tests-external/run_programs.sh` runs them as `directed-<name>` in
  `./run.sh tests-external [new]`. Run one directly with
  `build/tests-external/gemmini_diff tests-external/directed/<name>.trace`.
- The expected values come from libgemmini (`dependencies/libgemmini`) and the
  `gemmini-rocc-tests` macros (`dependencies/gemmini-rocc-tests/include/gemmini.h`).
  Neither is the official spec. The Gemmini README is incomplete, e.g. for config_mvout
  it only says "rs2 = the stride in bytes".
- Unrelated: the staged edits in `tests/test_*.cc` go back to the old ILA API, so
  `./run.sh tests new` does not compile.

## Done

- TODO 1: `dependencies/gemmini` is a submodule at `8c3f992` (master HEAD), which pins
  `gemmini-rocc-tests 7c540b3` and `libgemmini ea8f7ed`, the same as ours. It has a row in
  `tests-external/README.md`, and is not in `dependencies/CMakeLists.txt`.
- TODO 2: every directed trace has a `# Spec:` line. Bugs 4 and 6 are fully stated in the
  README, and bug 5 partly; the rest cite the RTL. The RTL agrees with libgemmini on all 8,
  so `test.md` needs no change. One related detail: in the default config,
  `mvin_scale_acc_args = None`, so the RTL does not scale non-shrunk accumulator mvins, and
  neither does libgemmini (`HAS_MVIN_ACC_SCALE` is not defined).

## Open

- `git submodule update --init --recursive dependencies` now also clones gemmini's own
  submodules, including the large `onnxruntime-riscv`.
