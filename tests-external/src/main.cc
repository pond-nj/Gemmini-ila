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
// How ILA state is matched to libgemmini state:
//   DRAM         ILA cell a (32 bits) holds the byte at address a, zero-extended.
//   scratchpad   ILA row index r is scratchpad row r.
//   accumulator  ILA row index 0x80000000 | r is accumulator row r (the local address
//                with bit 31 set, as the ILA stores it). Any other index is a stray write.
//
// Exit status: 0 all match, 1 first mismatch reported, 2 bad trace or libgemmini rejected it.

#include "golden.h"
#include "ila_sim.h"

#include <fstream>
#include <iostream>
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

// Adds one line per differing element of `what` row/cell `idx`.
void DiffRow(std::vector<std::string>& diffs, const std::string& what, uint64_t idx,
             const std::vector<int64_t>& ref, const std::vector<int64_t>& ila) {
  for (size_t c = 0; c < ref.size(); c++) {
    auto got = c < ila.size() ? ila[c] : 0;
    if (ref[c] != got)
      diffs.push_back(what + " " + Hex(idx) + (ref.size() > 1 ? " col " + std::to_string(c) : "") +
                      ": libgemmini " + std::to_string(ref[c]) + ", ILA " + std::to_string(got));
  }
}

std::vector<std::string> Compare(const Golden& ref, const IlaSim& ila) {
  std::vector<std::string> diffs;
  const size_t dim = ref.Dim();

  auto dram = ila.Dram();
  for (const auto& [addr, byte] : ref.Dram()) dram[addr];
  for (const auto& [addr, cell] : dram) {
    auto it = ref.Dram().find(addr);
    DiffRow(diffs, "DRAM", addr, {it == ref.Dram().end() ? 0 : it->second}, cell);
  }

  auto sp = ila.Scratchpad();
  for (const auto& [idx, row] : sp)
    if (idx >= ref.SpRows()) diffs.push_back("ILA wrote scratchpad index " + Hex(idx) + ", past the last row");
  for (size_t r = 0; r < ref.SpRows(); r++) {
    std::vector<int64_t> want(dim);
    for (size_t c = 0; c < dim; c++) want[c] = ref.Sp(r, c);
    auto it = sp.find(r);
    DiffRow(diffs, "scratchpad row", r, want, it == sp.end() ? std::vector<int64_t>(dim) : it->second);
  }

  std::map<uint64_t, std::vector<int64_t>> acc;
  for (const auto& [idx, row] : ila.Accumulator()) {
    if (idx >> 29 == 4 && (idx & 0x1FFFFFFF) < ref.AccRows()) acc[idx & 0x1FFFFFFF] = row;
    else diffs.push_back("ILA wrote accumulator index " + Hex(idx) + ", not an accumulator row address");
  }
  for (size_t r = 0; r < ref.AccRows(); r++) {
    std::vector<int64_t> want(dim);
    for (size_t c = 0; c < dim; c++) want[c] = ref.Acc(r, c);
    auto it = acc.find(r);
    DiffRow(diffs, "accumulator row", r, want, it == acc.end() ? std::vector<int64_t>(dim) : it->second);
  }
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
  IlaSim ila(ref.Dim());
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
      diffs = Compare(ref, ila);
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
  std::cout << "PASS " << path << " (" << count << " instructions, DIM " << ref.Dim() << ")\n";
  return 0;
}
