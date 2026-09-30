#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Runs the Gemmini ILA (../../src/gemmini.cc) on concrete values, one instruction at a time.
// Every state starts at 0. Kept free of gemmini.h, whose macros (mvin, config, ...) would
// leak into the includer.
class IlaSim {
public:
  explicit IlaSim(size_t dim);
  ~IlaSim();

  // Runs one official instruction: the start instruction whose decode holds, then the
  // *_step instruction whose decode holds, until none does. Returns the names run, empty if
  // nothing decodes. Throws if two instructions decode at once or the steps never stop.
  std::vector<std::string> Exec(unsigned funct, uint64_t rs1, uint64_t rs2);
  void WriteDram(uint64_t addr, uint8_t byte);

  // Cells written so far (index -> elements); every other cell is 0.
  using Cells = std::map<uint64_t, std::vector<int64_t>>;
  Cells Dram() const;        // one unsigned 32-bit element per cell
  Cells Scratchpad() const;  // DIM signed 8-bit elements per row
  Cells Accumulator() const; // DIM signed 32-bit elements per row

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
