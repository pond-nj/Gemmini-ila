# Gemmini ILA: differential test findings

Run on 2026-09-25 with `run.sh` (DIM 16), which compares the ILA against libgemmini
(ucb-bar/libgemmini ea8f7ed). The ILA source was not modified.

## Summary

- 1 of 21 gemmini-rocc-tests programs passes (`mvin_mvout_zeros`). All 40 random programs
  tried (seeds 1-40) fail.
- 8 bugs are confirmed: mvin (4), mvout (1), config_mvin (1) and decode (2). They make up
  1-8 in the table below.
- Every program that reaches a matmul compute fails there too (7 rocc tests). The causes
  of those are not confirmed yet (C1-C3).

"Confirmed" means a failing run, plus the ILA line that explains it. "Unconfirmed" means
the failure is real but its cause is a guess from reading `src/gemmini.cc`.

## Confirmed bugs

| # | Instruction | Bug | ILA line | Evidence |
|---|---|---|---|---|
| 1 | config_mvout | DRAM row stride is taken from all 64 bits of `rs2`. Only `rs2[31:0]` is the stride; `rs2[63:32]` is the float scale | `src/gemmini.cc:311` | aligned, mvin_mvout, mvin_mvout_stride, mvin_mvout_block_stride, padded |
| 2 | mvin (to accumulator, 32-bit) | Element `e` is read as one DRAM cell at `addr + e`. Officially it is 4 bytes at `addr + 4e` | `src/gemmini.cc:152-156` | mvin_mvout_acc, mvin_mvout_acc_stride, mvin_mvout_acc_zero_stride |
| 3 | mvin (to accumulator, shrunk int8) | The packed 8-bit row is zero-extended into the accumulator row. Each byte should become its own sign-extended 32-bit element | `src/gemmini.cc:175, 178` | global_average, matrix_add |
| 4 | mvin | Local-address flag bits `[31:29]` are not stripped, so writes land at raw index `0xa0000000+` | `src/gemmini.cc:106, 140` | mvin_mvout_acc_full, mvin_mvout_acc_full_stride |
| 5 | mvin | The mvin scale (`config_mvin rs1[63:32]`) is stored but never applied | `src/gemmini.cc:293` (state `scale` has no reader) | mvin_scale, matrix_add, random-4 |
| 6 | config_mvin / mvin | One stride for all loads. Officially each of the 3 load pipelines (`rs1[4:3]`, used by mvin, mvin2, mvin3) has its own stride, scale and shrunk flag | `src/gemmini.cc:290, 292` (`mvin_type` has no reader) | random-1 |
| 7 | mvin2 (funct 1) | Decodes as `matmul.compute.atomic`, which is not in the official ISA | `include/gemmini.h:32` | random seed 35 |
| 8 | mvin3 (funct 14) and every funct of 8 or more | The `funct` input is 3 bits wide, so these can't be expressed | `src/gemmini.cc:12` | random seed 3 |

### Evidence

Each block is the first difference `gemmini_diff` reported (first 1-2 differing cells shown).

**1. config_mvout stride** (`aligned`)
```
config_mvout rs2 = 0x3f80000000000010    stride 16, scale 1.0
MVOUT funct=3 rs1=0x00005bdf16edd001 rs2=0x0010001000000000
ILA ran: mvout + 17 x mvout_step
DRAM 0x5bdf16edd011: libgemmini 16, ILA 0        row 1 is missing: ILA stride = 0x3f80000000000010
```

**2. 32-bit mvin to the accumulator** (`mvin_mvout_acc`)
```
MVIN funct=2 rs1=0x00005aa546634100 rs2=0x0010001080000000
accumulator row 0x0 col 0: libgemmini 980276929, ILA 193    ILA got 1 byte, not 4
```

**3. shrunk mvin to the accumulator** (`global_average`; `config_mvin rs1=0x3f80000000010105`, shrunk = 1)
```
MVIN funct=2 rs1=0x0000587e1a5490a0 rs2=0x0001002f80000000
accumulator row 0x0 col 0: libgemmini 7, ILA 151257863     0x09040307 = bytes 07 03 04 09 packed
accumulator row 0x0 col 1: libgemmini 3, ILA 84279560
```

**4. flag bits in the mvin address** (`mvin_mvout_acc_full`)
```
MVIN funct=2 rs1=0x000063335fe90100 rs2=0x00100010a0000000   bit 29 set
ILA wrote accumulator index 0xa0000000, not an accumulator row address
```

**5. mvin scale ignored** (`mvin_scale`; `config_mvin rs1=0x0000000000100101`, scale 0.0)
```
MVIN funct=2 rs1=0x00007ffccf995220 rs2=0x0010001000000000
scratchpad row 0x0 col 1: libgemmini 0, ILA 1
```
random-4 (scale -1.0): `libgemmini 64, ILA -64`, `libgemmini -51, ILA 51`.

**6. per-pipeline config** (random-1)
```
config_mvin pipeline 0: rs1=0x3f80000000100001 rs2=0x1     stride 1
config_mvin pipeline 1: rs1=0x3f80000000100009 rs2=0x10    stride 16 (issued last)
mvin (pipeline 0) funct=2 rs1=0x00000000000126b5 rs2=0x0010001000000042
scratchpad row 0x43 col 0: libgemmini -52, ILA 59          ILA used stride 16
```

**7. mvin2** (random seed 35)
```
mvin2 funct=1 rs1=0x000000000001008b rs2=0x0010001080000000
ILA ran: matmul.compute.atomic
accumulator row 0x0 col 0: libgemmini 52, ILA 0
```

**8. mvin3** (random seed 3)
```
mvin3 funct=14 rs1=0x0000000000012700 rs2=0x0010001000000012
ILA ran: nothing (no ILA instruction decodes this)
```

## Compute failures, cause unconfirmed

The first compute of each program below fails. Bugs 1-6 are not involved: the mvins before
it match libgemmini.

| Program | First failing instruction | First differences |
|---|---|---|
| matmul_ws | `COMPUTE_PRELOADED rs1=0x0010001000000000 rs2=0x00100010ffffffff` (WS, D = GARBAGE, C to acc) | acc row 0 col 0: libgemmini 419, ILA 4003; col 1: 275 vs 55571 |
| matmul | `COMPUTE_PRELOADED rs1=0x0010001000000000 rs2=0x0010001000000020` | sp row 0x60 col 0: libgemmini 127, ILA -29; col 1: -128 vs 109 |
| matmul_os | same as matmul | sp row 0x60 col 0: libgemmini -128, ILA -28 |
| template | `COMPUTE_PRELOADED rs1=0x0010001000000000 rs2=0x0010001000000020` | sp row 0x14 col 14: libgemmini 71, ILA 0; row 0x15 col 13: 124 vs 0 |
| transpose | same as template | sp row 0x11 col 15: libgemmini 9, ILA 0; row 0x13 col 14: 3 vs 0 |
| raw_hazard | `COMPUTE_PRELOADED rs1=0x0010001000000020 rs2=0x0010001000000010` | sp rows 0x2b-0x2d, cols 11-13: libgemmini 2, ILA 1 |
| tiled_matmul_os | `COMPUTE_ACCUMULATE rs1=0x0010001000000030 rs2=0x0010001000003fc0` | acc row 0 col 0: libgemmini 18, ILA 4; col 1: 16 vs 2 |

Possible causes, from reading the code (none tested):

- **C1. Signedness and width.** The int8 operands are zero-extended before multiplying
  (`src/gemmini.cc:615, 807`), and the WS partial sum is cut to 16 bits (`:633, 825`). This
  fits matmul_ws: every ILA value is in 0..65535, while libgemmini's are small and signed.
- **C2. No saturation.** Results written to the scratchpad are truncated to 8 bits
  (`:655, 847`). libgemmini rounds the shift and saturates to int8, which fits matmul and
  matmul_os, where libgemmini shows 127 and -128.
- **C3. Too few steps.** The step decode stops after `2*DIM` cycles (`:547, 739`). In a
  skewed `DIM x DIM` array the last output arrives around cycle `3*DIM-2`, which fits
  template and transpose: the missing (0) cells are at the bottom-right.

To isolate compute from the mvin bugs, try `OPS=config_ex,matmul ./run.sh`.

## Not covered

- rocc tests that use funct 8 or more (`loop_ws`, `loop_conv_ws`, counters): conv*,
  tiled_matmul_ws*, resadd*, matmul_spad, mvin_mvout_spad, gemmini_counter. The ILA can't
  express these instructions (bug 8).
- Only the first difference per program is reported, so later bugs in the same program
  stay hidden until the earlier ones are fixed.
- libgemmini is the reference. Where it might differ from the RTL (rounding in
  `acc_scale`, for example), the RTL decides.

## Reproduce

```sh
cd tests-external
./run.sh                                                    # full run
build/gemmini_diff build/traces/aligned.trace               # one rocc test (after run.sh)
./gen_random.py --dim 16 --seed 35 > s35.trace && build/gemmini_diff s35.trace
```
