#pragma once

#include <cstdint>
#include <map>
#include <memory>

// The reference model: libgemmini, Gemmini's Spike functional model, on a Spike processor
// and MMU over sparse DRAM.
// Each Exec is one official instruction, applied atomically.
class Golden {
public:
  explicit Golden(bool log);
  ~Golden();

  // Throws std::runtime_error if libgemmini rejects the instruction.
  void Exec(unsigned funct, uint64_t rs1, uint64_t rs2);
  void WriteDram(uint64_t addr, uint8_t byte);

  // Bytes never written, or written as 0, are left out.
  std::map<uint64_t, uint8_t> Dram() const;
  size_t Dim() const;
  size_t SpRows() const;
  size_t AccRows() const;
  int64_t Sp(size_t row, size_t col) const;
  int64_t Acc(size_t row, size_t col) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
