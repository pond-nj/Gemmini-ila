#include "golden.h"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

// Every standard header gemmini.h pulls in must come before the `private` override.
#include <limits>
#include <random>
#include <riscv/extension.h>
#include <riscv/mmu.h>
#include <riscv/processor.h>
#include <riscv/rocc.h>
#include <riscv/simif.h>
#include <riscv/trap.h>

// libgemmini keeps its state private and has no accessors.
#define private public
#include "gemmini.h"
#undef private

namespace {

// Spike's view of DRAM: sparse pages, zero-filled on first touch, so any address is memory.
class SparseDram : public simif_t {
public:
  static const reg_t kPageSize = 4096;

  explicit SparseDram(const cfg_t& cfg) : cfg_(cfg) {}

  char* addr_to_mem(reg_t paddr) override {
    auto& page = pages_[paddr / kPageSize];
    if (page.empty()) page.resize(kPageSize);
    return page.data() + paddr % kPageSize;
  }
  bool mmio_load(reg_t, size_t, uint8_t*) override { return false; }
  bool mmio_store(reg_t, size_t, const uint8_t*) override { return false; }
  void proc_reset(unsigned) override {}
  const cfg_t& get_cfg() const override { return cfg_; }
  const std::map<size_t, processor_t*>& get_harts() const override { return harts_; }
  const char* get_symbol(uint64_t) override { return nullptr; }

  std::map<uint64_t, uint8_t> NonzeroBytes() const {
    std::map<uint64_t, uint8_t> bytes;
    for (const auto& [page, data] : pages_)
      for (reg_t i = 0; i < kPageSize; i++)
        if (data[i]) bytes[page * kPageSize + i] = data[i];
    return bytes;
  }

private:
  const cfg_t& cfg_;
  std::map<size_t, processor_t*> harts_;
  std::map<reg_t, std::vector<char>> pages_;
};

} // namespace

struct Golden::Impl {
  cfg_t cfg;
  SparseDram dram{cfg};
  processor_t proc{cfg.isa, cfg.priv, &cfg, &dram, 0, false, stderr, std::cerr};
  gemmini_t gem;
};

// make_unique value-initializes Impl, so libgemmini's plain config fields start at 0.
Golden::Golden(bool log) : impl_(std::make_unique<Impl>()) {
  if (log) impl_->proc.enable_log_commits();
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
  try {
    impl_->gem.CUSTOMFN(XCUSTOM_ACC)(insn, rs1, rs2);
  } catch (trap_t& t) {
    throw std::runtime_error("libgemmini: " + t.name());
  }
}

void Golden::WriteDram(uint64_t addr, uint8_t byte) { *impl_->dram.addr_to_mem(addr) = byte; }

std::map<uint64_t, uint8_t> Golden::Dram() const { return impl_->dram.NonzeroBytes(); }
size_t Golden::Dim() const { return DIM; }
size_t Golden::SpRows() const { return impl_->gem.gemmini_state.spad.size(); }
size_t Golden::AccRows() const { return impl_->gem.gemmini_state.accumulator.size(); }
int64_t Golden::Sp(size_t row, size_t col) const { return impl_->gem.gemmini_state.spad[row][col]; }
int64_t Golden::Acc(size_t row, size_t col) const { return impl_->gem.gemmini_state.accumulator[row][col]; }
