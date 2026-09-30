// Minimal stand-in for Spike's riscv/extension.h: just enough for libgemmini to
// compile and run outside Spike. The harness drives gemmini_t::custom3 directly.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

typedef uint64_t reg_t;

class processor_t;

struct insn_t {
  uint64_t b;
  uint64_t rd() const { return (b >> 7) & 0x1f; }
  uint64_t rs1() const { return (b >> 15) & 0x1f; }
  uint64_t rs2() const { return (b >> 20) & 0x1f; }
};

typedef reg_t (*insn_func_t)(processor_t*, insn_t, reg_t);
struct insn_desc_t {};
struct disasm_insn_t {};

class extension_t {
public:
  virtual ~extension_t() {}
  virtual const char* name() const = 0;
  virtual std::vector<insn_desc_t> get_instructions(const processor_t& p) = 0;
  virtual std::vector<disasm_insn_t*> get_disasms(const processor_t* p) = 0;
};

#define REGISTER_EXTENSION(name, constructor)
