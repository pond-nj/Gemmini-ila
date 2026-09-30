// Host-side shim for gemmini-rocc-tests: each Gemmini RoCC instruction becomes a trace
// line on stderr instead of running. Before each mvin it also prints the DRAM bytes the
// mvin reads (".mem <addr> <hex>"), so the trace can be replayed without the program.
#include_next "rocc-software/src/xcustom.h"
#include <stdint.h>
#include <stdio.h>

static const char *gemmini_trace_name(int f) {
  switch (f) {
    case 0: return "CONFIG"; case 1: return "MVIN2"; case 2: return "MVIN"; case 3: return "MVOUT";
    case 4: return "COMPUTE_PRELOADED"; case 5: return "COMPUTE_ACCUMULATE"; case 6: return "PRELOAD";
    case 7: return "FLUSH"; case 8: return "LOOP_WS"; case 9: return "LOOP_WS_CONFIG_BOUNDS";
    case 10: return "LOOP_WS_CONFIG_ADDRS_AB"; case 11: return "LOOP_WS_CONFIG_ADDRS_DC";
    case 12: return "LOOP_WS_CONFIG_STRIDES_AB"; case 13: return "LOOP_WS_CONFIG_STRIDES_DC";
    case 14: return "MVIN3"; case 15: return "LOOP_CONV_WS"; case 16: return "LOOP_CONV_WS_CONFIG_1";
    case 17: return "LOOP_CONV_WS_CONFIG_2"; case 18: return "LOOP_CONV_WS_CONFIG_3";
    case 19: return "LOOP_CONV_WS_CONFIG_4"; case 20: return "LOOP_CONV_WS_CONFIG_5";
    case 21: return "LOOP_CONV_WS_CONFIG_6"; case 23: return "MVOUT_SPAD";
    case 24: return "LOOP_WS_CONFIG_SPAD_AB"; case 25: return "LOOP_WS_CONFIG_SPAD_C"; case 126: return "COUNTER";
  }
  return "UNKNOWN";
}

// Per load pipeline (mvin, mvin2, mvin3): DRAM row stride and "shrunk" (int8 into the
// accumulator), as config_mvin sets them.
static uint64_t gemmini_trace_stride[3];
static int gemmini_trace_shrunk[3];

// Mirrors libgemmini's mvin: rows x cols elements, 4 bytes each into the accumulator
// unless shrunk, 1 byte otherwise. DRAM address 0 means zeros, nothing is read.
static void gemmini_trace_mvin_bytes(int id, uint64_t dram, uint64_t local) {
  uint64_t cols = (local >> 32) & 0xFFFF, rows = (local >> 48) & 0xFFFF;
  uint64_t elem = ((local >> 31) & 1) && !gemmini_trace_shrunk[id] ? 4 : 1;
  if (dram == 0) return;
  for (uint64_t r = 0; r < rows; r++) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)(dram + r * gemmini_trace_stride[id]);
    fprintf(stderr, ".mem 0x%llx ", (unsigned long long)(uintptr_t)p);
    for (uint64_t b = 0; b < cols * elem; b++) fprintf(stderr, "%02x", p[b]);
    fprintf(stderr, "\n");
  }
}

static void gemmini_trace(int f, uint64_t rs1, uint64_t rs2) {
  int id = f == 2 ? 0 : f == 1 ? 1 : f == 14 ? 2 : -1;
  if (f == 0 && (rs1 & 3) == 1) {
    gemmini_trace_stride[(rs1 >> 3) & 3] = rs2;
    gemmini_trace_shrunk[(rs1 >> 3) & 3] = (rs1 >> 2) & 1;
  }
  if (id >= 0) gemmini_trace_mvin_bytes(id, rs1, rs2);
  fprintf(stderr, "%-26s funct=%-3d rs1=0x%016llx rs2=0x%016llx\n", gemmini_trace_name(f), f,
          (unsigned long long)rs1, (unsigned long long)rs2);
}

#undef ROCC_INSTRUCTION_0_R_R
#define ROCC_INSTRUCTION_0_R_R(x, rs1, rs2, func7) \
  { gemmini_trace((func7), (uint64_t)(rs1), (uint64_t)(rs2)); }

#undef ROCC_INSTRUCTION_R_R_R
#define ROCC_INSTRUCTION_R_R_R(x, rd, rs1, rs2, func7) \
  { ROCC_INSTRUCTION_0_R_R(x, rs1, rs2, func7) rd = 0; }

// No accelerator on the host: output checks fail, so keep going instead of exiting.
#define exit(c) fprintf(stderr, "# exit(%d) ignored\n", (int)(c))

// RISC-V-only inline asm used by the headers.
__asm__(".macro fence\n.endm\n.macro rdcycle r\nxorq \\r, \\r\n.endm\n");
