#pragma once

#include "gemmini_model.h"

#include <memory>

// The reference model: libgemmini, Gemmini's Spike functional model, on a Spike processor
// and MMU over sparse DRAM.
// Each Exec is one official instruction, applied atomically.
class Golden : public GemminiModel {
public:
  explicit Golden(bool log);
  ~Golden() override;

  // Throws std::runtime_error if libgemmini rejects the instruction. Returns no steps.
  std::vector<std::string> Exec(unsigned funct, uint64_t rs1, uint64_t rs2) override;
  void WriteDram(uint64_t addr, uint8_t byte) override;
  ArchState State() const override;
  Geometry Shape() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
