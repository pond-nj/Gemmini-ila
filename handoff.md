# Handoff: remaining Gemmini ILA differential-test failures

Status as of 2026-09-29. Goal: make `src/gemmini.cc` match the official Gemmini ISA so
`tests-external/run.sh` passes. Use only instructions from the official ISA.

## Summary

- `SEEDS=40 tests-external/run.sh`: 61 of 61 pass (21 rocc tests + 40 random seeds). The run
  takes about 10 min; each DIM-16 compute is 47 ILA steps (~0.5 s).
- M1-M5 (mvout) and C1-C7 (compute) below are fixed (uncommitted). See "Fixed on 2026-09-29".

## Fixed on 2026-09-29

- mvout: `mvout_step` (scratchpad) and `mvout_acc_step` (accumulator) are separate, so the
  scratchpad path stays small for `IlaZ3Unroller`. Flag bits [30:26] stripped from the source row; blocks of DIM columns for `cols > DIM`;
  accumulator reads scaled by `acc_scale` (config_mvout `rs2[63:32]`) with `FloatScale`
  (float32, RNE, saturate), then ReLU if `acc_activation` (config_mvout `rs1[3:2]`) is 1;
  full-width reads (bit 29) write 4 little-endian bytes, unscaled; every DRAM cell is one byte.
- config_ex: `activation_func` is `rs1[4:3]`, `c_stride` is `rs2[63:48]`, and
  `set_only_strides` (`rs1[7]`) updates only the strides.
- matmul.preload only records the D/B (`preload_*`) and C (`dest_*`) operands.
  matmul.compute.preloaded loads the PEs, as libgemmini and the RTL do.
- Compute (`AddSteppedCompute`, one function for preloaded and accumulated): signed int8
  operands, 32-bit partial sums, skewed systolic timing (PE (r, c) works on
  `k` or `i = cycle - r - c`), the last PE finishes at cycle `3*DIM-3`, and cycle `3*DIM-2`
  writes C. C is written only at the end, so C may overlap A, B or D. WS results wait in the
  new `result_reg` per PE. GARBAGE B/D/A read zeros. Output row `i` goes to
  `C[28:0] + c_stride*i`. The accumulator gets the raw sum (bit 30: added). The scratchpad gets
  `RoundShiftRightEven` (now arithmetic) by `right_shift` (OS only), saturated, then ReLU.
- The atomic model (`AddAtomicCompute`) uses the same element and write-back helpers.
- Unit tests updated: 5 compute steps at DIM 2, the preload test checks `preload_*`,
  `config_ex` sets `c_stride` 1 (as `gemmini_config_ex` does), and the mvin test pins
  `funct` to mvin and checks column 0 only. All 18 pass.
- Still open: `verifyComputeAtomicVsStepped` reports "NOT equivalent". It does not constrain
  `done`, `preload_*`, `dest_row`, `dest_col` or `c_stride` to be equal in both models at step 0.
- `IlaZ3Unroller` walks expressions as trees (no visited set). Keep repeated subterms out of
  step updates. `RoundToFloat32` takes the value's real width for this reason.

## References (cross-check every change)

| Source | Path |
|---|---|
| Official ISA (README, "Memory Addressing Scheme" and "ISA") | `~/project/ila/gemmini/README.md` |
| Official software encodings | `tests-external/dependencies/gemmini-rocc-tests/include/gemmini.h` |
| RTL (decides when libgemmini disagrees) | `~/project/ila/chipyard/generators/gemmini/src/main/scala/gemmini/` (`GemminiISA.scala`, `LoadController.scala`, `StoreController.scala`, `ExecuteController.scala`, `Configs.scala`) |
| Reference model used by the harness | `tests-external/dependencies/libgemmini/gemmini.cc` (`mvout` ~l.225, `config` ~l.374, `compute` ~l.892, `acc_scale`/`sys_shift` ~l.1848) |

## Build and run

`cmake` is not on PATH. Use `~/project/ila/visualize/env/bin/cmake`.

```sh
cd tests-external
~/project/ila/visualize/env/bin/cmake --build build -j      # rebuilds libgemmini_ila.a + gemmini_diff
SEEDS=40 ./run.sh                                           # ~1 min, per-program reports in build/results/
build/gemmini_diff build/traces/mvin_mvout_acc.trace        # one program
OPS=config_ex,matmul ./run.sh                               # compute only, no mvout/mvin noise
```

The top-level `CMakeLists.txt` expects `ILAng/` in the repo, which is not there. To run the
repo's own unit tests (`src/main.cc`, 18 tests), build them against the installed ILAng with
`ilang_DIR=~/project/ila/visualize/install/lib/cmake/ilang` and
`Z3_LIBRARY=~/project/ila/visualize/env/lib/libz3.so`. All 18 pass now.
`verifyComputeAtomicVsStepped` still reports "NOT equivalent". That was already the case
before any change and is marked "Not done" in the code.

Harness conventions (`tests-external/src/main.cc`):
- DRAM: ILA cell `a` (32 bits) holds the byte at address `a`, zero-extended.
- Accumulator row `r` must be stored at ILA index `0x80000000 | r`. Any other index with
  bit 31 set (e.g. `0xa...`, `0xc...`) is reported as a stray write.

## Already done (uncommitted, in `include/gemmini.h`, `src/gemmini.cc`, `src/main.cc`, `tests/`)

- `funct` is 7 bits (`FUNCT_WIDTH`). `mvin2` = 1, `mvin3` = 14.
- Load config is per pipeline: `acc_type[3]`, `private_stride[3]`, `memory_stride_mvin[3]`,
  `scale[3]`. `mvin_pipeline` records which mvin started. `mvin_type` was removed.
- `mvin_step` rewritten: 4-byte little-endian acc elements, sign-extended shrunk elements,
  flag bits [30:29] stripped, bit 30 accumulates, float mvin scale (`MvinScale()` in
  `gemmini.h`), untouched columns preserved, DRAM address 0 loads zeros.
- config_mvout stride = `rs2[31:0]`.
- `matmul.compute.atomic` no longer takes funct 1. It is built only with
  `Gemmini(cfg, name, Gemmini::ComputeModel::Atomic)`, where it decodes funct 4 instead of
  the stepped compute.
- Helpers you can reuse: `RoundShiftRightEven(v, shift)` (RNE right shift) and
  `MvinScale(x, float_bits)` (float32 multiply, RNE to int, saturate) in `include/gemmini.h`.

## Failure 1: mvout from the accumulator (15 programs)

Programs: `global_average`, `matrix_add`, `mvin_mvout_acc`, `mvin_mvout_acc_full`,
`mvin_mvout_acc_full_stride`, `mvin_mvout_acc_stride`, `mvin_mvout_acc_zero_stride`,
random 2, 14, 21, 27, 30, 35, 37, 39. Typical: `DRAM ...: libgemmini 127, ILA 0`.

| # | Cause | ILA location | Fix |
|---|---|---|---|
| M1 | Scales with `scalar` (config_ex `rs1[63:32]`) as an integer multiply, then keeps 8 bits. `0x3f800000 * x` has 23 zero low bits, so the result is always 0 | `src/gemmini.cc:245` (`ScaleBv`), `:282` | Add state `acc_scale` (32) set by config_mvout from `rs2[63:32]` (`ConfigMvoutRs2` in `GemminiISA.scala`). Scale as float32, RNE, saturate to int8. `MvinScale()` does this for int8 input. Write an int32-input version (the product needs up to 56 bits, so widen `W` and the excess-bit loop) |
| M2 | No activation on accumulator reads | config_mvout (`:304`) stores no activation | Store config_mvout `rs1[3:2]` (0 none, 1 ReLU. See libgemmini `config` ~l.455 for 2 and 3). Apply ReLU after scaling |
| M3 | Flag bit 29 is not stripped from the source address, so it reads index `0xa000...` | `:232` (`sour_base`) | Row index = `bit31 : 00 : addr[28:0] + row`. libgemmini strips [31:26] for mvout. Bits [28:26] are the norm command |
| M4 | Full-width read (bit 29) not modelled | `:241-253` | If bit 29 is set: no scale and no activation. Write 4 bytes per element at `dram_row + 4*col` (little-endian, one byte per DRAM cell) |
| M5 | Each element is written as one 32-bit cell, not bytes | `:249-253` | Byte-sized: `Extract(v, 7, 0)` per cell (scratchpad mvout already passes with this) |

Check also: `cols > DIM` for mvout (libgemmini uses `block = j / DIM`, block stride DIM
rows). The ILA mvout only covers `elem < DIM`.

## Failure 2: compute (39 programs)

Programs: `matmul`, `matmul_os`, `matmul_ws`, `raw_hazard`, `template`, `transpose`,
`tiled_matmul_os`, and 32 random seeds. The same causes apply to both
`matmul.compute.preloaded_step` (`:551-713`) and `matmul.compute.accumulated_step`
(`:743-905`). The two are copies, so fix both, and fix the atomic model (`:371-521`) too so
the equivalence check stays meaningful.

| # | Cause | ILA location | Symptom | Fix |
|---|---|---|---|---|
| C1 | int8 operands are zero-extended | `:620-621`, `:812-813`, `:631` (psum/D) | `matmul_ws`: 4003 vs 419 | `SExt` A, B and D to 32 bits |
| C2 | WS partial sum cut to 16 bits (`OUTPUT_BITS`); `C_reg_out` is 16 bits | `:639`, `:831`, `PE` in `gemmini.h` | acc values in 0..65535, e.g. 58158 vs -4562 | Keep the psum at `ACC_BITS` (32) |
| C3 | No rounding shift or saturation on scratchpad writes | OS `:694-695`, `:886-887` (`>>` is `Ashr`, then truncate); WS writes low 8 bits | `matmul`: -29 vs 127 | libgemmini `sys_shift`: `ROUNDING_RIGHT_SHIFT(value, shift)` (shift = `right_shift` in OS, 0 in WS), then saturate to [-128, 127], then ReLU if config_ex `rs1[3]` |
| C4 | Too few steps: decode stops at `cycle <= 2*DIM-1`. A skewed DIM x DIM array needs about `3*DIM-2` cycles for the last output | `:553`, `:556`, `:713` and the accumulated copies | `template`, `transpose`: bottom-right cells stay 0 | Raise the bound and move the OS `write_cycle` to the last cycle. Check the result against the WS `col_valid`/`instance` logic (`:645-651`) |
| C5 | Accumulate flag not stripped from the output address, and no accumulate | `:651`, `:683`, `:843`, `:875`; `dest_addr` set at `:332` | `ILA wrote accumulator index 0xc000...` (11 random seeds) | Index = `bit31 : 00 : addr[28:0] + row`. If bit 30 is set, add to the old accumulator value. libgemmini: `acc_accum` in `compute` |
| C6 | D = GARBAGE (`0xffffffff`) is read as data: `B_D_addr + k` wraps to rows 0, 1, ... | `:631` (psum_in), B feed `:600-610` | D picks up real data (`matmul_ws`) | If `B_D_addr == 0xffffffff`, D/B = 0 (libgemmini: `~bd_addr_real == 0 ? 0`). Same for A |
| C7 | `c_stride` ignored: output rows always consecutive | config_ex `:273-283` does not store `rs2[63:48]` | random seeds with `c_stride` 2 | Add state `c_stride` (16). Output row `i` goes to `base + c_stride * i` |

C1 to C4 were C1 to C3 in `test.md`. Their code locations are confirmed. C5 to C7 are new.

To check: in the ILA the preloaded and accumulated steps are identical. Compare them with
the ISA text in the README (`matmul.compute.accumulated`: OS keeps accumulating in the PEs,
WS reuses the previous weights) and with libgemmini `compute(..., preload)`. Also check
whether a D/B address with bit 31 set may be read from the accumulator.

## Suggested order

1. C5 and C6: small, they remove stray writes and garbage data. Then rerun with `OPS=config_ex,matmul`.
2. C1 and C2: widths and sign.
3. C3: rounding shift and saturation.
4. C4: step count (the biggest change, touches the timing of both dataflows).
5. C7.
6. M1 to M5 (independent of compute; can be done in parallel).
7. Rerun the repo unit tests. The 2x2 matmul tests in `tests/test_matmul.cc` use
   non-negative values, so they should still pass after C1. Recheck after C4, because they
   hard-code the step counts.

## Pitfalls

- libgemmini is not always right. Its `ROUND_NEAR_EVEN` returns `x+-1` instead of `i+-1`
  and uses UB for `(long long)` of huge floats. So for odd scales (e.g. `-0.49999997`) or
  overflows (e.g. `1e30`) it differs from the RTL (hardfloat `RecFNToIN`, RNE, saturate by
  sign). The RTL decides. `ACC_SCALE` uses the same macro, so expect the same thing for M1.
  `gen_random.py` only uses scales 1.0, 0.5, 2.0, 0.0 and -1.0, which are safe.
- In ILAng, `<`/`>` are signed unless `UnsignedComparison` is set, and `>>` is arithmetic.
  Use `Ult`/`Uge`/`Ugt`/`Lshr` where unsigned is meant.
- The ILA simulator (`tests-external/src/ila_sim.cc`) evaluates the whole update
  expression. Large per-element expressions (like `MvinScale` x DIM) are fine; compute steps
  are the slow part.
- `run.sh` reports only the first difference per program. Fixing one cause can uncover
  the next one in the same program.
