#pragma once

#include "gemmini_model.h"

#include <memory>

// Runs the Gemmini ILA (../../src/gemmini.cc) on concrete values, one instruction at a time.
// Every state starts at 0. Kept free of gemmini.h, whose macros (mvin, config, ...) would
// leak into the includer.
//
// ILA state as architectural state:
//   DRAM         cell a (32 bits) holds the byte at address a, zero-extended.
//   scratchpad   row index r is scratchpad row r.
//   accumulator  row index 0x80000000 | r is accumulator row r (the local address with
//                bit 31 set). Any other index, or a row past `shape`, is a stray write.
class IlaSim : public GemminiModel {
public:
  explicit IlaSim(Geometry shape);
  ~IlaSim() override;

  // Runs the start instruction whose decode holds, then the *_step instruction whose decode
  // holds, until none does. Returns the names run, empty if nothing decodes. Throws if two
  // instructions decode at once or the steps never stop.
  std::vector<std::string> Exec(unsigned funct, uint64_t rs1, uint64_t rs2) override;
  void WriteDram(uint64_t addr, uint8_t byte) override;
  ArchState State() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
