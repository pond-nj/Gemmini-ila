// Differential test: runs one Gemmini program on libgemmini (the reference) and on the
// Gemmini ILA, and compares DRAM, scratchpad and accumulator after every instruction.
//
//   gemmini_diff [--log] <trace>      (trace "-" reads stdin)
//
// Trace lines (hosttrace/trace.sh and gen_random.py both write this):
//   <NAME> funct=<n> rs1=<v> rs2=<v>   one official Gemmini instruction; NAME is ignored
//   .mem <addr> <hex bytes>            CPU writes these bytes to DRAM, starting at addr
//   # ... / // ...                     comment
//
// Both models report their state as ArchState (gemmini_model.h); ila_sim.h says how ILA
// state maps to it.
//
// Exit status: 0 all match, 1 first mismatch reported, 2 bad trace or libgemmini rejected it.

#include "golden.h"
#include "ila_sim.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace {

const size_t kMaxShown = 20;

struct Insn {
  unsigned funct;
  uint64_t rs1, rs2;
};

bool ParseInsn(const std::string& line, Insn& insn) {
  std::istringstream in(line);
  bool f = false, a = false, b = false;
  for (std::string tok; in >> tok;) {
    auto eq = tok.find('=');
    if (eq == std::string::npos) continue;
    auto key = tok.substr(0, eq);
    auto val = std::stoull(tok.substr(eq + 1), nullptr, 0);
    if (key == "funct") insn.funct = val, f = true;
    if (key == "rs1") insn.rs1 = val, a = true;
    if (key == "rs2") insn.rs2 = val, b = true;
  }
  return f && a && b;
}

std::string Hex(uint64_t v) {
  std::ostringstream s;
  s << "0x" << std::hex << v;
  return s.str();
}

// Adds one line per differing element of `what` row/cell `idx`. Missing elements are 0.
void DiffRow(std::vector<std::string>& diffs, const std::string& what, uint64_t idx,
             const std::vector<int64_t>& ref, const std::vector<int64_t>& ila) {
  size_t width = std::max(ref.size(), ila.size());
  for (size_t c = 0; c < width; c++) {
    auto want = c < ref.size() ? ref[c] : 0;
    auto got = c < ila.size() ? ila[c] : 0;
    if (want != got)
      diffs.push_back(what + " " + Hex(idx) + (width > 1 ? " col " + std::to_string(c) : "") +
                      ": libgemmini " + std::to_string(want) + ", ILA " + std::to_string(got));
  }
}

std::vector<int64_t> Elems(int64_t v) { return {v}; }
const std::vector<int64_t>& Elems(const std::vector<int64_t>& row) { return row; }

// Diffs every index either model holds. A missing index is all 0.
template <typename Map>
void DiffAll(std::vector<std::string>& diffs, const std::string& what, const Map& ref, const Map& ila) {
  std::set<uint64_t> idxs;
  for (const auto& kv : ref) idxs.insert(kv.first);
  for (const auto& kv : ila) idxs.insert(kv.first);
  for (auto idx : idxs) {
    auto r = ref.find(idx), i = ila.find(idx);
    DiffRow(diffs, what, idx, r == ref.end() ? std::vector<int64_t>{} : Elems(r->second),
            i == ila.end() ? std::vector<int64_t>{} : Elems(i->second));
  }
}

std::vector<std::string> Compare(const ArchState& ref, const ArchState& ila) {
  std::vector<std::string> diffs;
  for (const auto& s : ref.stray) diffs.push_back("libgemmini " + s);
  for (const auto& s : ila.stray) diffs.push_back("ILA " + s);
  DiffAll(diffs, "DRAM", ref.dram, ila.dram);
  DiffAll(diffs, "scratchpad row", ref.spad, ila.spad);
  DiffAll(diffs, "accumulator row", ref.acc, ila.acc);
  return diffs;
}

std::string Summary(const std::vector<std::string>& ran) {
  if (ran.empty()) return "nothing (no ILA instruction decodes this)";
  std::string s = ran[0];
  if (ran.size() > 1) s += " + " + std::to_string(ran.size() - 1) + " x " + ran[1];
  return s;
}

} // namespace

int main(int argc, char** argv) {
  bool log = false;
  std::string path;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--log") log = true;
    else path = a;
  }
  if (path.empty()) {
    std::cerr << "usage: gemmini_diff [--log] <trace>\n";
    return 2;
  }
  std::ifstream file;
  if (path != "-") file.open(path);
  std::istream& in = path == "-" ? std::cin : file;
  if (!in) {
    std::cerr << "cannot open " << path << "\n";
    return 2;
  }

  Golden ref(log);
  IlaSim ila(ref.Shape());
  int line_no = 0, count = 0;
  for (std::string line; std::getline(in, line);) {
    line_no++;
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#' || line.compare(first, 2, "//") == 0) continue;

    if (line.compare(first, 5, ".mem ") == 0) {
      std::istringstream ws(line.substr(first + 5));
      std::string addr_s, bytes;
      ws >> addr_s >> bytes;
      uint64_t addr = std::stoull(addr_s, nullptr, 0);
      for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        auto b = static_cast<uint8_t>(std::stoul(bytes.substr(i, 2), nullptr, 16));
        ref.WriteDram(addr + i / 2, b);
        ila.WriteDram(addr + i / 2, b);
      }
      continue;
    }

    Insn insn{};
    if (!ParseInsn(line, insn)) {
      std::cerr << path << ":" << line_no << ": cannot parse: " << line << "\n";
      return 2;
    }
    try {
      ref.Exec(insn.funct, insn.rs1, insn.rs2);
    } catch (const std::exception& e) {
      std::cerr << path << ":" << line_no << ": " << e.what() << ": " << line << "\n";
      return 2;
    }

    std::vector<std::string> ran, diffs;
    try {
      ran = ila.Exec(insn.funct, insn.rs1, insn.rs2);
      diffs = Compare(ref.State(), ila.State());
    } catch (const std::exception& e) {
      diffs.push_back(e.what());
    }
    count++;
    if (diffs.empty()) continue;

    std::cout << "FAIL " << path << ":" << line_no << " (instruction " << count << ")\n"
              << "  " << line.substr(first) << "\n"
              << "  ILA ran: " << Summary(ran) << "\n";
    for (size_t i = 0; i < diffs.size() && i < kMaxShown; i++) std::cout << "  " << diffs[i] << "\n";
    if (diffs.size() > kMaxShown) std::cout << "  ... and " << diffs.size() - kMaxShown << " more\n";
    return 1;
  }
  std::cout << "PASS " << path << " (" << count << " instructions, DIM " << ref.Shape().dim << ")\n";
  return 0;
}
