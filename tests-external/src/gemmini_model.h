#pragma once

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Systolic array size of both models: the DIM libgemmini and the rocc-tests are written for.
constexpr size_t kDim = 16;

struct Geometry {
  size_t sp_rows, acc_rows;
};

// Index (address or row) -> its elements.
using Rows = std::map<uint64_t, std::vector<int64_t>>;

// Architectural state, in the same terms for every model. Anything left out is 0.
struct ArchState {
  Rows dram;                      // byte address -> {byte}
  Rows spad;                      // row -> DIM elements
  Rows acc;                       // row -> DIM elements
  std::vector<std::string> stray; // writes to no architectural location
};

inline std::string Hex(uint64_t v) {
  std::ostringstream s;
  s << "0x" << std::hex << v;
  return s.str();
}

// A Gemmini model that runs official instructions on DRAM the CPU wrote.
class GemminiModel {
public:
  virtual ~GemminiModel() = default;

  // Runs one official instruction. Returns the model's internal steps, for diagnostics.
  virtual std::vector<std::string> Exec(unsigned funct, uint64_t rs1, uint64_t rs2) = 0;
  virtual void WriteDram(uint64_t addr, uint8_t byte) = 0;
  virtual ArchState State() const = 0;
};
