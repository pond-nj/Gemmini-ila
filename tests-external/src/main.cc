// Differential test: runs one Gemmini program on libgemmini (the reference) and
// on the Gemmini ILA, and compares DRAM, scratchpad and accumulator after every
// instruction.
//
//   gemmini_diff [--log] [--progress <file>] <trace>      (trace "-" reads stdin)
//
// --progress writes one line per instruction to <file>, written before the
// instruction runs and finished after, so a slow instruction shows as in flight.
//
// Trace lines (hosttrace/trace.sh and gen_random.py both write this):
//   <NAME> funct=<n> rs1=<v> rs2=<v>   one official Gemmini instruction; NAME
//   is ignored .mem <addr> <hex bytes>            CPU writes these bytes to
//   DRAM, starting at addr # ...                              comment
//
// Both models report their state as ArchState (gemmini_model.h); ila_sim.h says
// how ILA state maps to it.
//
// Exit status: 0 all match, 1 first mismatch reported, 2 bad trace or
// libgemmini rejected it.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "golden.h"
#include "ila_sim.h"

namespace {

constexpr size_t kMaxShownDifferences = 20;

struct Options {
  bool log = false;
  std::string trace_path;
  std::string progress_path;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; i++) {
    std::string argument = argv[i];
    if (argument == "--log")
      options.log = true;
    else if (argument == "--progress" && i + 1 < argc)
      options.progress_path = argv[++i];
    else
      options.trace_path = argument;
  }
  return options;
}

struct Insn {
  unsigned funct;
  uint64_t rs1, rs2;
};

bool ParseInsn(const std::string& line, Insn& insn) {
  std::istringstream tokens(line);
  bool has_funct = false, has_rs1 = false, has_rs2 = false;
  for (std::string token; tokens >> token;) {
    auto separator = token.find('=');
    if (separator == std::string::npos) continue;
    auto key = token.substr(0, separator);
    auto value = std::stoull(token.substr(separator + 1), nullptr, 0);
    if (key == "funct") {
      insn.funct = value;
      has_funct = true;
    } else if (key == "rs1") {
      insn.rs1 = value;
      has_rs1 = true;
    } else if (key == "rs2") {
      insn.rs2 = value;
      has_rs2 = true;
    }
  }
  return has_funct && has_rs1 && has_rs2;
}

void ApplyMemoryWrite(const std::string& contents, GemminiModel& reference,
                      GemminiModel& ila) {
  std::istringstream tokens(contents);
  std::string address_text, bytes;
  tokens >> address_text >> bytes;
  uint64_t address = std::stoull(address_text, nullptr, 0);
  for (size_t offset = 0; offset + 1 < bytes.size(); offset += 2) {
    auto byte =
        static_cast<uint8_t>(std::stoul(bytes.substr(offset, 2), nullptr, 16));
    reference.WriteDram(address + offset / 2, byte);
    ila.WriteDram(address + offset / 2, byte);
  }
}

// Adds one line per differing element of `what` row/cell `idx`. Missing
// elements are 0.
void DiffRow(std::vector<std::string>& diffs, const std::string& what,
             uint64_t idx, const std::vector<int64_t>& ref,
             const std::vector<int64_t>& ila) {
  size_t width = std::max(ref.size(), ila.size());
  for (size_t column = 0; column < width; column++) {
    auto expected = column < ref.size() ? ref[column] : 0;
    auto actual = column < ila.size() ? ila[column] : 0;
    if (expected != actual)
      diffs.push_back(what + " " + Hex(idx) +
                      (width > 1 ? " col " + std::to_string(column) : "") +
                      ": libgemmini " + std::to_string(expected) + ", ILA " +
                      std::to_string(actual));
  }
}

const std::vector<int64_t>& RowAt(const Rows& rows, uint64_t idx) {
  static const std::vector<int64_t> kZeroRow;
  auto row = rows.find(idx);
  return row == rows.end() ? kZeroRow : row->second;
}

// Diffs every index either model holds. A missing index is all 0.
void DiffAll(std::vector<std::string>& diffs, const std::string& what,
             const Rows& ref, const Rows& ila) {
  std::set<uint64_t> indices;
  for (const auto& row : ref) indices.insert(row.first);
  for (const auto& row : ila) indices.insert(row.first);
  for (auto index : indices)
    DiffRow(diffs, what, index, RowAt(ref, index), RowAt(ila, index));
}

std::vector<std::string> Compare(const ArchState& ref, const ArchState& ila) {
  std::vector<std::string> diffs;
  for (const auto& write : ila.stray) diffs.push_back("ILA " + write);
  DiffAll(diffs, "DRAM", ref.dram, ila.dram);
  DiffAll(diffs, "scratchpad row", ref.spad, ila.spad);
  DiffAll(diffs, "accumulator row", ref.acc, ila.acc);
  return diffs;
}

std::string Summary(const std::vector<std::string>& executed_steps) {
  if (executed_steps.empty())
    return "nothing (no ILA instruction decodes this)";
  std::string summary = executed_steps[0];
  if (executed_steps.size() > 1)
    summary += " + " + std::to_string(executed_steps.size() - 1) + " x " +
               executed_steps[1];
  return summary;
}

struct InstructionResult {
  std::vector<std::string> executed_steps;
  std::vector<std::string> differences;
};

InstructionResult ExecuteIlaAndCompare(const Insn& insn,
                                       GemminiModel& reference,
                                       GemminiModel& ila) {
  auto reference_state = reference.State();
  if (!reference_state.stray.empty())
    throw std::logic_error("libgemmini reported a stray write: " +
                           reference_state.stray.front());
  InstructionResult result;
  try {
    result.executed_steps = ila.Exec(insn.funct, insn.rs1, insn.rs2);
    result.differences = Compare(reference_state, ila.State());
  } catch (const std::exception& error) {
    result.differences.push_back(error.what());
  }
  return result;
}

struct TracePosition {
  std::string path;
  int line_number = 0;
  int instruction_count = 0;
};

void ReportMismatch(const TracePosition& position, const std::string& line,
                    const InstructionResult& result) {
  std::cout << "FAIL " << position.path << ":" << position.line_number
            << " (instruction " << position.instruction_count << ")\n"
            << "  " << line << "\n"
            << "  ILA ran: " << Summary(result.executed_steps) << "\n";
  const auto& differences = result.differences;
  for (size_t i = 0; i < differences.size() && i < kMaxShownDifferences; i++)
    std::cout << "  " << differences[i] << "\n";
  if (differences.size() > kMaxShownDifferences) {
    std::cout << "  ... and " << differences.size() - kMaxShownDifferences
              << " more\n";
  }
}

int SimulateAndCompare(std::istream& input, std::ostream& progress,
                       const Options& options) {
  Golden reference(options.log);
  IlaSim ila(reference.Shape());
  TracePosition position{options.trace_path};
  for (std::string line; std::getline(input, line);) {
    position.line_number++;
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;

    if (line.compare(first, 5, ".mem ") == 0) {
      ApplyMemoryWrite(line.substr(first + 5), reference, ila);
      continue;
    }

    Insn insn{};
    if (!ParseInsn(line, insn)) {
      std::cerr << position.path << ":" << position.line_number
                << ": cannot parse: " << line << "\n";
      return 2;
    }
    position.instruction_count++;
    progress << "instruction " << position.instruction_count << " (line "
             << position.line_number << "): " << line.substr(first) << " -> "
             << std::flush;
    try {
      reference.Exec(insn.funct, insn.rs1, insn.rs2);
    } catch (const std::exception& error) {
      std::cerr << position.path << ":" << position.line_number << ": "
                << error.what() << ": " << line << "\n";
      return 2;
    }

    auto result = ExecuteIlaAndCompare(insn, reference, ila);
    progress << "ILA ran " << Summary(result.executed_steps) << std::endl;
    if (result.differences.empty()) continue;

    ReportMismatch(position, line.substr(first), result);
    return 1;
  }
  std::cout << "PASS " << position.path << " (" << position.instruction_count
            << " instructions, DIM " << reference.Shape().dim << ")\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto options = ParseOptions(argc, argv);
  if (options.trace_path.empty()) {
    std::cerr << "usage: gemmini_diff [--log] [--progress <file>] <trace>\n";
    return 2;
  }
  std::ifstream file;
  if (options.trace_path != "-") file.open(options.trace_path);
  std::istream& input = options.trace_path == "-" ? std::cin : file;
  if (!input) {
    std::cerr << "cannot open " << options.trace_path << "\n";
    return 2;
  }
  // Left closed without --progress, so progress writes are dropped.
  std::ofstream progress;
  if (!options.progress_path.empty()) {
    progress.open(options.progress_path);
    if (!progress) {
      std::cerr << "cannot write " << options.progress_path << "\n";
      return 2;
    }
  }
  return SimulateAndCompare(input, progress, options);
}
