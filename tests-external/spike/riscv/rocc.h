// Minimal stand-in for Spike's riscv/rocc.h (see extension.h).
#pragma once

#include "extension.h"

struct rocc_insn_t {
  unsigned opcode : 7;
  unsigned rd : 5;
  unsigned xs2 : 1;
  unsigned xs1 : 1;
  unsigned xd : 1;
  unsigned rs1 : 5;
  unsigned rs2 : 5;
  unsigned funct : 7;
};

union rocc_insn_union_t {
  rocc_insn_t r;
  insn_t i;
};

#define ROCC_OPCODE3 0x7b
#define ROCC_OPCODE_MASK 0x7f
#define ILLEGAL_INSN_FUNC nullptr

inline void push_custom_insn(std::vector<insn_desc_t>&, reg_t, reg_t, insn_func_t, insn_func_t) {}
