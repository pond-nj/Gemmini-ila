#include "golden.h"

#include <algorithm>
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

// libgemmini's reset banner must not appear in differential-test results.
class ScopedStdoutSilencer {
public:
  ScopedStdoutSilencer() {
    std::fflush(stdout);
    saved_stdout_ = dup(STDOUT_FILENO);
    freopen("/dev/null", "w", stdout);
  }

  ~ScopedStdoutSilencer() {
    std::fflush(stdout);
    dup2(saved_stdout_, STDOUT_FILENO);
    close(saved_stdout_);
  }

  ScopedStdoutSilencer(const ScopedStdoutSilencer&) = delete;
  ScopedStdoutSilencer& operator=(const ScopedStdoutSilencer&) = delete;

private:
  int saved_stdout_;
};

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
    for (const auto& [page_index, page_bytes] : pages_)
      for (reg_t offset = 0; offset < kPageSize; offset++)
        if (page_bytes[offset]) bytes[page_index * kPageSize + offset] = page_bytes[offset];
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
  // libgemmini's DRAM accesses (mvin/mvout) go through proc's MMU, which resolves them via
  // dram.addr_to_mem, so Gemmini's own stores land in dram without calling WriteDram.
  processor_t proc{cfg.isa, cfg.priv, &cfg, &dram, 0, false, stderr, std::cerr};
  gemmini_t gem;
};

// make_unique value-initializes Impl, so libgemmini's plain config fields start at 0.
Golden::Golden(bool log) : impl_(std::make_unique<Impl>()) {
  if (log) impl_->proc.enable_log_commits();
  impl_->gem.set_processor(&impl_->proc);

  ScopedStdoutSilencer silence_banner;
  impl_->gem.reset();
}

Golden::~Golden() = default;

std::vector<std::string> Golden::Exec(unsigned funct, uint64_t rs1, uint64_t rs2) {
  rocc_insn_t insn{};
  insn.funct = funct;
  insn.xs1 = insn.xs2 = 1;
  try {
    impl_->gem.CUSTOMFN(XCUSTOM_ACC)(insn, rs1, rs2);
  } catch (trap_t& t) {
    throw std::runtime_error("libgemmini: " + t.name());
  }
  return {};
}

void Golden::WriteDram(uint64_t addr, uint8_t byte) { *impl_->dram.addr_to_mem(addr) = byte; }

namespace {

template <typename Table>
Rows NonzeroRows(const Table& rows) {
  Rows nonzero_rows;
  for (size_t index = 0; index < rows.size(); index++) {
    const auto& row = rows[index];
    if (std::any_of(row.begin(), row.end(), [](auto value) { return value != 0; })) {
      nonzero_rows[index].assign(row.begin(), row.end());
    }
  }
  return nonzero_rows;
}

} // namespace

ArchState Golden::State() const {
  const auto& gemmini_state = impl_->gem.gemmini_state;
  ArchState state;
  for (const auto& [address, byte] : impl_->dram.NonzeroBytes()) state.dram[address] = {byte};
  state.spad = NonzeroRows(gemmini_state.spad);
  state.acc = NonzeroRows(gemmini_state.accumulator);
  return state;
}

Geometry Golden::Shape() const {
  const auto& gemmini_state = impl_->gem.gemmini_state;
  return {DIM, gemmini_state.spad.size(), gemmini_state.accumulator.size()};
}
