# tests-external

Differential tests for the Gemmini ILA (`../src/gemmini.cc`). Each program runs on the ILA
and on [libgemmini](https://github.com/ucb-bar/libgemmini), Gemmini's Spike functional model,
used as the reference. DRAM, scratchpad and accumulator are compared after every official
instruction, and the run stops at the first difference.

| Path | What it is |
| --- | --- |
| `src/main.cc` | `gemmini_diff`: replays a trace on both models and compares them |
| `src/gemmini_model.h` | the interface both models implement, and the state they report |
| `src/golden.*` | libgemmini on a Spike processor and MMU, over sparse DRAM |
| `src/ila_sim.*` | runs the ILA on concrete values (start instruction, then its `*_step`s) |
| `hosttrace/` | turns a `gemmini-rocc-tests` program into a trace, on the host |
| `gen_random.py` | writes constrained-random traces |
| `run.sh` | runs both kinds and prints PASS/FAIL per program |
| `dependencies/` | submodules: `gemmini-rocc-tests`, `libgemmini`, `riscv-isa-sim` (Spike) |

## Build

Needs an installed ILAng (`ilangConfig.cmake`), Z3, and `dtc` (device-tree-compiler, required by
Spike's configure). Spike is built from the submodule into `<build>/spike` on the first build.

```sh
git submodule update --init --recursive tests-external/dependencies
cmake -S tests-external -B tests-external/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="<ilang install>;<z3 install>"
cmake --build tests-external/build -j
```

Both models are built with DIM 16, the size libgemmini and the rocc-tests are written for.
`run.sh` builds for you.

## Run

```sh
tests-external/run.sh                                  # rocc-tests + 20 random programs
tests-external/run.sh new                              # same, built against new/src, new/include
SEEDS=100 OPS=mvin,mvout,config_mvin,config_mvout tests-external/run.sh
tests-external/build/gemmini_diff prog.trace           # one program; --log adds libgemmini's log
tests-external/hosttrace/trace.sh matmul_ws out.trace  # trace one rocc test (x86-64 gcc)
tests-external/gen_random.py --dim 16 --seed 3 --ops mvin,mvout > out.trace
```

Use `OPS` to test a few instructions at a time, so one broken instruction doesn't hide
bugs in the others.

A failure looks like this:

```
FAIL out.trace:274 (instruction 17)
  mvin                   funct=2   rs1=0x00000000000126b5 rs2=0x0010001000000042
  ILA ran: mvin + 16 x mvin_step
  scratchpad row 0x43 col 0: libgemmini -52, ILA 59
```

## Trace format

```
<NAME> funct=<n> rs1=<v> rs2=<v>   one official instruction (NAME is ignored)
.mem <addr> <hex bytes>            CPU writes these bytes to DRAM
# comment
```

## How ILA state is matched to libgemmini state

- DRAM: ILA cell `a` (32 bits) holds the byte at address `a`, zero-extended.
- Scratchpad: ILA row `r` is scratchpad row `r`.
- Accumulator: ILA row `0x80000000 | r` is accumulator row `r`, which is the local
  address as the ILA stores it. Any other index, such as one with the accumulate or
  full-width flag bit set, is reported as a stray write.

## Limits

- Only architectural state is compared. Config registers and PE registers are internal
  to each model.
- Both models start with every state at 0.
- The ILA's `funct` input is 3 bits wide, so instructions with funct 8 or more (loops,
  counters) can't reach it. The rocc tests in `run.sh` are the ones that don't use them.
- A host trace records CPU memory only where an mvin reads it, at the time of that mvin.
- libgemmini is itself a model. If it looks wrong, check the RTL
  (`generators/gemmini/src/main/scala/gemmini`).
- `ila_sim.cc` converts ILA expressions to Z3 with its own memoized walk.
  `IlaZ3Unroller::GetZ3Expr` takes time exponential in DIM on the compute steps: over
  100 s per instruction at DIM 4.
