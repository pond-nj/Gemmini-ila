#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Geometry {
  size_t dim, sp_rows, acc_rows;
};

// Architectural state, in the same terms for every model. Anything left out is 0.
struct ArchState {
  std::map<uint64_t, int64_t> dram;                  // byte address -> byte
  std::map<uint64_t, std::vector<int64_t>> spad;     // row -> DIM elements
  std::map<uint64_t, std::vector<int64_t>> acc;      // row -> DIM elements
  std::vector<std::string> stray;                    // writes to no architectural location
};

// A Gemmini model that runs official instructions on DRAM the CPU wrote.
class GemminiModel {
public:
  virtual ~GemminiModel() = default;

  // Runs one official instruction. Returns the model's internal steps, for diagnostics.
  virtual std::vector<std::string> Exec(unsigned funct, uint64_t rs1, uint64_t rs2) = 0;
  virtual void WriteDram(uint64_t addr, uint8_t byte) = 0;
  virtual ArchState State() const = 0;
};
