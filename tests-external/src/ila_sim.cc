#include "ila_sim.h"

#include "gemmini.h"

#include <ilang/ila/ast/expr.h>

#include <stdexcept>
#include <unordered_map>

using namespace gemmini;

namespace {

const int kMaxSteps = 1 << 20;

bool IsStep(const std::string& name) {
  return name.size() > 5 && name.compare(name.size() - 5, 5, "_step") == 0;
}

z3::expr Zero(z3::context& c, const z3::sort& s) {
  if (s.is_bool()) return c.bool_val(false);
  if (s.is_bv()) return c.bv_val(0, s.bv_size());
  return z3::const_array(s.array_domain(), c.bv_val(0, s.array_range().bv_size()));
}

// ILA expression -> z3, memoized per node. IlaZ3Unroller::GetZ3Expr walks the expression
// DAG as a tree (Expr::DepthFirstVisit keeps no visited set), which is exponential in DIM
// for the compute steps: over 100 s per instruction at DIM 4.
class Z3Translator {
public:
  explicit Z3Translator(z3::context& ctx) : ctx_(ctx) {}

  z3::expr operator()(const ExprRef& root) {
    std::vector<std::pair<ExprPtr, bool>> stack{{root.get(), false}};
    while (!stack.empty()) {
      auto [e, children_done] = stack.back();
      stack.pop_back();
      if (done_.count(e.get())) continue;
      if (children_done) {
        std::vector<z3::expr> args;
        for (size_t i = 0; i < e->arg_num(); i++) args.push_back(done_.at(e->arg(i).get()));
        done_.emplace(e.get(), e->GetZ3Expr(ctx_, args, ""));
        continue;
      }
      stack.push_back({e, true});
      for (size_t i = 0; i < e->arg_num(); i++) stack.push_back({e->arg(i), false});
    }
    return done_.at(root.get().get());
  }

private:
  z3::context& ctx_;
  std::unordered_map<const Expr*, z3::expr> done_;
};

} // namespace

// Each state holds a concrete z3 value. A step substitutes the current values into the
// instruction's update functions and lets z3's simplifier fold them to new values.
// Memories are kept as a map of written cells and rebuilt as const-array + stores.
struct IlaSim::Impl {
  struct Instr {
    std::string name;
    z3::expr decode;
    std::vector<size_t> targets; // state indices this instruction updates
    z3::expr_vector updates;
  };

  z3::context ctx;
  Gemmini gem;
  Z3Translator z3_of;
  std::vector<ExprRef> states;
  z3::expr_vector vars;   // states, then inputs, at time 0
  std::vector<z3::expr> values; // current value of each var
  size_t funct_in, rs1_in, rs2_in;
  std::map<size_t, std::map<uint64_t, z3::expr>> mems; // state index -> written cells
  size_t dram;
  bool dram_dirty = false;
  std::vector<Instr> instrs;

  explicit Impl(size_t dim)
      : gem(cfg(dim, dim, 1, 1, 4, 64 * 1024, 128 * 1024, DataType::INT8, DataType::INT16, DataType::INT32),
            "gemmini"),
        z3_of(ctx), vars(ctx) {
    gem.AddInstructions();
    Ila& m = gem.get();
    for (size_t i = 0; i < m.state_num(); i++) {
      states.push_back(m.state(i));
      vars.push_back(z3_of(m.state(i)));
      values.push_back(Zero(ctx, vars.back().get_sort()));
      if (vars.back().is_array()) mems[i];
      if (m.state(i).name() == gem.DRAM.name()) dram = i;
    }
    for (size_t i = 0; i < m.input_num(); i++) {
      auto name = m.input(i).name();
      (name == "funct" ? funct_in : name == "rs1" ? rs1_in : rs2_in) = vars.size();
      vars.push_back(z3_of(m.input(i)));
      values.push_back(Zero(ctx, vars.back().get_sort()));
    }
    for (size_t i = 0; i < m.instr_num(); i++) {
      auto instr = m.instr(i);
      Instr in{instr.name(), z3_of(instr.GetDecode()), {}, z3::expr_vector(ctx)};
      for (size_t s = 0; s < states.size(); s++) {
        auto update = instr.GetUpdate(states[s]);
        if (update.get().get() == nullptr) continue;
        in.targets.push_back(s);
        in.updates.push_back(z3_of(update));
      }
      instrs.push_back(in);
    }
  }

  // Evaluates `exprs` on the current values, in one z3 call so shared subterms fold once.
  z3::expr_vector Eval(const z3::expr_vector& exprs) {
    z3::sort_vector sorts(ctx);
    for (const auto& e : exprs) sorts.push_back(e.get_sort());
    auto pack = ctx.function("pack", sorts, ctx.bool_sort())(exprs);
    z3::expr_vector current(ctx);
    for (const auto& v : values) current.push_back(v);
    auto folded = pack.substitute(vars, current).simplify();
    z3::expr_vector out(ctx);
    for (unsigned i = 0; i < folded.num_args(); i++) out.push_back(folded.arg(i));
    return out;
  }

  void Rebuild(size_t s) {
    auto v = Zero(ctx, vars[s].get_sort());
    for (const auto& [addr, data] : mems[s]) v = z3::store(v, ctx.bv_val(addr, vars[s].get_sort().array_domain().bv_size()), data);
    values[s] = v;
  }

  // Reads the written cells back out of a folded memory value (outermost store wins).
  void Collect(size_t s) {
    std::map<uint64_t, z3::expr> cells;
    auto e = values[s];
    while (e.is_app() && e.decl().decl_kind() == Z3_OP_STORE) {
      if (!e.arg(1).is_numeral() || !e.arg(2).is_numeral())
        throw std::runtime_error("ILA: " + states[s].name() + " did not fold to constants");
      cells.emplace(e.arg(1).get_numeral_uint64(), e.arg(2));
      e = e.arg(0);
    }
    if (!(e.is_app() && e.decl().decl_kind() == Z3_OP_CONST_ARRAY))
      throw std::runtime_error("ILA: unexpected value for " + states[s].name() + ": " + e.to_string().substr(0, 200));
    mems[s] = cells;
  }

  void Apply(const Instr& in) {
    auto next = Eval(in.updates);
    for (size_t i = 0; i < in.targets.size(); i++) values[in.targets[i]] = next[i];
    for (auto s : in.targets)
      if (mems.count(s)) {
        Collect(s);
        Rebuild(s);
      }
  }

  // Instructions whose decode holds, among the steps (`step`) or the starts.
  std::vector<const Instr*> Decoded(bool step) {
    std::vector<const Instr*> cands;
    z3::expr_vector decodes(ctx);
    for (const auto& in : instrs)
      if (IsStep(in.name) == step) {
        cands.push_back(&in);
        decodes.push_back(in.decode);
      }
    auto held = Eval(decodes);
    std::vector<const Instr*> out;
    for (size_t i = 0; i < cands.size(); i++)
      if (held[i].is_true()) out.push_back(cands[i]);
    if (out.size() > 1) {
      std::string names;
      for (auto in : out) names += " " + in->name;
      throw std::runtime_error("ILA: ambiguous decode:" + names);
    }
    return out;
  }

  Cells Read(const ExprRef& mem, unsigned elem_bits, bool is_signed) const {
    size_t s = 0;
    while (states[s].name() != mem.name()) s++;
    Cells out;
    for (const auto& [addr, data] : mems.at(s)) {
      auto& row = out[addr];
      unsigned width = data.get_sort().bv_size();
      for (unsigned lo = 0; lo < width; lo += elem_bits) {
        auto v = static_cast<int64_t>(width <= 64 ? data.get_numeral_uint64() >> lo & ((uint64_t(2) << (elem_bits - 1)) - 1)
                                                  : data.extract(lo + elem_bits - 1, lo).simplify().get_numeral_uint64());
        if (is_signed && elem_bits < 64 && (v >> (elem_bits - 1)) & 1) v -= int64_t(1) << elem_bits;
        row.push_back(v);
      }
    }
    return out;
  }
};

IlaSim::IlaSim(size_t dim) : impl_(std::make_unique<Impl>(dim)) {}
IlaSim::~IlaSim() = default;

std::vector<std::string> IlaSim::Exec(unsigned funct, uint64_t rs1, uint64_t rs2) {
  auto& d = *impl_;
  if (d.dram_dirty) {
    d.Rebuild(d.dram);
    d.dram_dirty = false;
  }
  std::vector<std::string> ran;
  unsigned funct_bits = d.vars[d.funct_in].get_sort().bv_size();
  if (funct >> funct_bits) return ran; // does not fit the ILA's funct input
  d.values[d.funct_in] = d.ctx.bv_val(funct, funct_bits);
  d.values[d.rs1_in] = d.ctx.bv_val(rs1, RS_WIDTH);
  d.values[d.rs2_in] = d.ctx.bv_val(rs2, RS_WIDTH);

  auto start = d.Decoded(false);
  if (start.empty()) return ran;
  d.Apply(*start[0]);
  ran.push_back(start[0]->name);
  for (int n = 0;; n++) {
    auto step = d.Decoded(true);
    if (step.empty()) break;
    if (n == kMaxSteps) throw std::runtime_error("ILA: " + step[0]->name + " still decodes after " + std::to_string(n) + " steps");
    d.Apply(*step[0]);
    ran.push_back(step[0]->name);
  }
  return ran;
}

void IlaSim::WriteDram(uint64_t addr, uint8_t byte) {
  auto& d = *impl_;
  d.mems[d.dram].insert_or_assign(addr, d.ctx.bv_val(byte, DRAM_DATA_WIDTH));
  d.dram_dirty = true;
}

IlaSim::Cells IlaSim::Dram() const { return impl_->Read(impl_->gem.DRAM, DRAM_DATA_WIDTH, false); }
IlaSim::Cells IlaSim::Scratchpad() const { return impl_->Read(impl_->gem.scratchpad, INPUT_TYPE_BIT_WIDTH, true); }
IlaSim::Cells IlaSim::Accumulator() const { return impl_->Read(impl_->gem.accumulator, ACC_TYPE_BIT_WIDTH, true); }
