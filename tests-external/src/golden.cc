#include "golden.h"

#include <cstdio>
#include <unistd.h>

// Every standard header gemmini.h pulls in must come before the `private` override.
#include <limits>
#include <random>
#include <riscv/extension.h>
#include <riscv/mmu.h>
#include <riscv/rocc.h>
#include <riscv/trap.h>

// libgemmini keeps its state private and has no accessors.
#define private public
#include "gemmini.h"
#undef private

struct Golden::Impl {
  processor_t proc;
  gemmini_t gem;
};

// make_unique value-initializes Impl, so libgemmini's plain config fields start at 0.
Golden::Golden(bool log) : impl_(std::make_unique<Impl>()) {
  impl_->proc.log_commits = log;
  impl_->proc.ext = &impl_->gem;
  impl_->gem.set_processor(&impl_->proc);

  // reset() prints a banner on stdout; keep stdout for results.
  std::fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  freopen("/dev/null", "w", stdout);
  impl_->gem.reset();
  std::fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
}

Golden::~Golden() = default;

void Golden::Exec(unsigned funct, uint64_t rs1, uint64_t rs2) {
  rocc_insn_t insn{};
  insn.funct = funct;
  insn.xs1 = insn.xs2 = 1;
  impl_->gem.CUSTOMFN(XCUSTOM_ACC)(insn, rs1, rs2);
}

void Golden::WriteDram(uint64_t addr, uint8_t byte) { impl_->proc.mmu.mem[addr] = byte; }

const std::map<uint64_t, uint8_t>& Golden::Dram() const { return impl_->proc.mmu.mem; }
size_t Golden::Dim() const { return DIM; }
size_t Golden::SpRows() const { return impl_->gem.gemmini_state.spad.size(); }
size_t Golden::AccRows() const { return impl_->gem.gemmini_state.accumulator.size(); }
int64_t Golden::Sp(size_t row, size_t col) const { return impl_->gem.gemmini_state.spad[row][col]; }
int64_t Golden::Acc(size_t row, size_t col) const { return impl_->gem.gemmini_state.accumulator[row][col]; }
