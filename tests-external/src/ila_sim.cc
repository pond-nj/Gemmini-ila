#include "ila_sim.h"

#include "gemmini.h"

#include <ilang/ila/ast/expr.h>

#include <optional>
#include <stdexcept>
#include <unordered_map>

using namespace gemmini;

namespace {

constexpr int kMaxSteps = 1 << 20;
constexpr uint64_t kAccumulatorAddressTag = uint64_t{1} << 31;
constexpr uint64_t kAccumulatorRowMask = (uint64_t{1} << 29) - 1;

enum class Signedness { Unsigned, Signed };

cfg CreateConfig(size_t dimension) {
  cfg model_config;
  model_config.tileRows = dimension;
  model_config.tileColumns = dimension;
  model_config.meshRows = 1;
  model_config.meshColumns = 1;
  model_config.DIM = dimension;
  model_config.sp_banks = 4;
  model_config.sp_capacity = 64 * 1024;
  model_config.acc_capacity = 128 * 1024;
  model_config.inputType = DataType::INT8;
  model_config.outputType = DataType::INT16;
  model_config.accType = DataType::INT32;
  return model_config;
}

std::optional<uint64_t> DecodeAccumulatorRow(uint64_t address, size_t row_count) {
  if ((address & ~kAccumulatorRowMask) != kAccumulatorAddressTag) return std::nullopt;
  uint64_t row = address & kAccumulatorRowMask;
  if (row >= row_count) return std::nullopt;
  return row;
}

uint64_t ExtractElement(const z3::expr& packed_row, unsigned bit_offset, unsigned element_bits) {
  if (packed_row.get_sort().bv_size() <= 64) {
    uint64_t mask = (uint64_t{2} << (element_bits - 1)) - 1;
    return (packed_row.get_numeral_uint64() >> bit_offset) & mask;
  }
  return packed_row.extract(bit_offset + element_bits - 1, bit_offset).simplify().get_numeral_uint64();
}

int64_t SignExtendElement(int64_t value, unsigned element_bits) {
  if (element_bits < 64 && ((value >> (element_bits - 1)) & 1))
    value -= int64_t{1} << element_bits;
  return value;
}

bool IsStepInstruction(const std::string& name) {
  return name.size() > 5 && name.compare(name.size() - 5, 5, "_step") == 0;
}

z3::expr Zero(z3::context& context, const z3::sort& sort) {
  if (sort.is_bool()) return context.bool_val(false);
  if (sort.is_bv()) return context.bv_val(0, sort.bv_size());
  return z3::const_array(sort.array_domain(), context.bv_val(0, sort.array_range().bv_size()));
}

// ILA expression -> z3, memoized per node. IlaZ3Unroller::GetZ3Expr walks the expression
// DAG as a tree (Expr::DepthFirstVisit keeps no visited set), which is exponential in DIM
// for the compute steps: over 100 s per instruction at DIM 4.
class Z3Translator {
public:
  explicit Z3Translator(z3::context& context) : context_(context) {}

  z3::expr operator()(const ExprRef& root) {
    std::vector<std::pair<ExprPtr, bool>> stack{{root.get(), false}};
    while (!stack.empty()) {
      auto [node, children_done] = stack.back();
      stack.pop_back();
      if (translated_nodes_.count(node.get())) continue;
      if (children_done) {
        std::vector<z3::expr> arguments;
        for (size_t i = 0; i < node->arg_num(); i++)
          arguments.push_back(translated_nodes_.at(node->arg(i).get()));
        translated_nodes_.emplace(node.get(), node->GetZ3Expr(context_, arguments, ""));
        continue;
      }
      stack.push_back({node, true});
      for (size_t i = 0; i < node->arg_num(); i++) stack.push_back({node->arg(i), false});
    }
    return translated_nodes_.at(root.get().get());
  }

private:
  z3::context& context_;
  std::unordered_map<const Expr*, z3::expr> translated_nodes_;
};

} // namespace

// Each state holds a concrete z3 value. A step substitutes the current values into the
// instruction's update functions and lets z3's simplifier fold them to new values.
// Memories are kept as const-array + stores, compacted after each update.
struct IlaSim::Impl {
  // An ILA instruction with its decode and updates translated to z3 once.
  struct TranslatedInstruction {
    std::string name;
    z3::expr decode;
    std::vector<size_t> updated_state_indices; // state indices this instruction updates
    z3::expr_vector updates;
  };

  Geometry shape;
  z3::context context;
  Gemmini gemmini_model;
  Z3Translator translate_expression;
  std::vector<ExprRef> states;
  z3::expr_vector variable_symbols;     // states, then inputs, at time 0
  std::vector<z3::expr> variable_values; // current value of each var
  size_t funct_input, rs1_input, rs2_input;
  size_t dram_state, scratchpad_state, accumulator_state;
  std::vector<TranslatedInstruction> start_instructions, step_instructions;

  explicit Impl(Geometry shape_)
      : shape(shape_), gemmini_model(CreateConfig(shape.dim), "gemmini"),
        translate_expression(context), variable_symbols(context) {
    gemmini_model.AddInstructions();
    Ila& model = gemmini_model.get();
    RegisterStates(model);
    RegisterInputs(model);
    TranslateInstructions(model);
  }

  // Registers each state as a var with a zero value.
  void RegisterStates(Ila& model) {
    for (size_t i = 0; i < model.state_num(); i++) {
      states.push_back(model.state(i));
      variable_symbols.push_back(translate_expression(model.state(i)));
      variable_values.push_back(Zero(context, variable_symbols.back().get_sort()));
      auto name = model.state(i).name();
      if (name == gemmini_model.DRAM.name()) dram_state = i;
      if (name == gemmini_model.scratchpad.name()) scratchpad_state = i;
      if (name == gemmini_model.accumulator.name()) accumulator_state = i;
    }
  }

  // Registers each input as a var after the states, and records funct/rs1/rs2.
  void RegisterInputs(Ila& model) {
    for (size_t i = 0; i < model.input_num(); i++) {
      auto name = model.input(i).name();
      if (name == "funct") funct_input = variable_symbols.size();
      else if (name == "rs1") rs1_input = variable_symbols.size();
      else rs2_input = variable_symbols.size();
      variable_symbols.push_back(translate_expression(model.input(i)));
      variable_values.push_back(Zero(context, variable_symbols.back().get_sort()));
    }
  }

  // Translates each instruction's decode and state updates to z3. Needs RegisterStates first.
  void TranslateInstructions(Ila& model) {
    for (size_t i = 0; i < model.instr_num(); i++) {
      auto instruction = model.instr(i);
      TranslatedInstruction translated{
          instruction.name(), translate_expression(instruction.GetDecode()), {}, z3::expr_vector(context)};
      for (size_t state_index = 0; state_index < states.size(); state_index++) {
        auto update = instruction.GetUpdate(states[state_index]);
        if (update.get().get() == nullptr) continue;
        translated.updated_state_indices.push_back(state_index);
        translated.updates.push_back(translate_expression(update));
      }
      auto& group = IsStepInstruction(instruction.name()) ? step_instructions : start_instructions;
      group.push_back(translated);
    }
  }

  // Evaluate together so Z3 folds shared subterms only once.
  z3::expr_vector Evaluate(const z3::expr_vector& expressions) {
    z3::sort_vector sorts(context);
    for (const auto& expression : expressions) sorts.push_back(expression.get_sort());
    auto pack_function = context.function("pack", sorts, context.bool_sort());
    auto packed_expressions = pack_function(expressions);
    z3::expr_vector current_values(context);
    for (const auto& value : variable_values) current_values.push_back(value);
    auto folded = packed_expressions.substitute(variable_symbols, current_values).simplify();
    z3::expr_vector results(context);
    for (unsigned i = 0; i < folded.num_args(); i++) results.push_back(folded.arg(i));
    return results;
  }

  // Reads the written cells out of a folded memory value (outermost store wins).
  std::map<uint64_t, z3::expr> ReadCells(size_t state_index) const {
    std::map<uint64_t, z3::expr> cells;
    auto memory = variable_values[state_index];
    while (memory.is_app() && memory.decl().decl_kind() == Z3_OP_STORE) {
      if (!memory.arg(1).is_numeral() || !memory.arg(2).is_numeral())
        throw std::runtime_error("ILA: " + states[state_index].name() + " did not fold to constants");
      cells.emplace(memory.arg(1).get_numeral_uint64(), memory.arg(2));
      memory = memory.arg(0);
    }
    bool is_fully_known = memory.is_app() && memory.decl().decl_kind() == Z3_OP_CONST_ARRAY;
    if (!is_fully_known)
      throw std::runtime_error("ILA: unexpected value for " + states[state_index].name() + ": " +
                               memory.to_string().substr(0, 200));
    return cells;
  }

  // Rebuilds an array with one store per written cell.
  void CompactArray(size_t state_index) {
    auto sort = variable_symbols[state_index].get_sort();
    auto memory = Zero(context, sort);
    for (const auto& [address, value] : ReadCells(state_index)) {
      auto address_expr = context.bv_val(address, sort.array_domain().bv_size());
      memory = z3::store(memory, address_expr, value);
    }
    variable_values[state_index] = memory;
  }

  void ApplyUpdates(const TranslatedInstruction& instruction) {
    auto next_values = Evaluate(instruction.updates);
    for (size_t i = 0; i < instruction.updated_state_indices.size(); i++)
      variable_values[instruction.updated_state_indices[i]] = next_values[i];
    // simplify() doesn't reliably drop overwritten stores. For example,
    // store(store(store(m, a, x), b, y), a, z) can keep the dead write (a, x). Across thousands
    // of steps, rows that get written again and again would make the chain grow without limit.
    for (auto state_index : instruction.updated_state_indices)
      if (variable_symbols[state_index].is_array()) CompactArray(state_index);
  }

  std::vector<const TranslatedInstruction*> DecodeInstructions(
      const std::vector<TranslatedInstruction>& candidates) {
    z3::expr_vector decodes(context);
    for (const auto& instruction : candidates) decodes.push_back(instruction.decode);
    auto decode_results = Evaluate(decodes);
    std::vector<const TranslatedInstruction*> decoded;
    for (size_t i = 0; i < candidates.size(); i++)
      if (decode_results[i].is_true()) decoded.push_back(&candidates[i]);
    if (decoded.size() > 1) {
      std::string names;
      for (auto instruction : decoded) names += " " + instruction->name;
      throw std::runtime_error("ILA: ambiguous decode:" + names);
    }
    return decoded;
  }

  Rows ReadRows(size_t state_index, unsigned element_bits, Signedness signedness) const {
    Rows rows;
    for (const auto& [address, packed_row] : ReadCells(state_index)) {
      auto& row = rows[address];
      unsigned row_bits = packed_row.get_sort().bv_size();
      for (unsigned bit_offset = 0; bit_offset < row_bits; bit_offset += element_bits) {
        auto value = static_cast<int64_t>(ExtractElement(packed_row, bit_offset, element_bits));
        if (signedness == Signedness::Signed) value = SignExtendElement(value, element_bits);
        row.push_back(value);
      }
    }
    return rows;
  }
};

IlaSim::IlaSim(Geometry shape) : impl_(std::make_unique<Impl>(shape)) {}
IlaSim::~IlaSim() = default;

std::vector<std::string> IlaSim::Exec(unsigned funct, uint64_t rs1, uint64_t rs2) {
  auto& simulator = *impl_;
  std::vector<std::string> executed_steps;
  unsigned funct_bits = simulator.variable_symbols[simulator.funct_input].get_sort().bv_size();
  if (funct >> funct_bits) return executed_steps; // does not fit the ILA's funct input
  simulator.variable_values[simulator.funct_input] = simulator.context.bv_val(funct, funct_bits);
  simulator.variable_values[simulator.rs1_input] = simulator.context.bv_val(rs1, RS_WIDTH);
  simulator.variable_values[simulator.rs2_input] = simulator.context.bv_val(rs2, RS_WIDTH);

  auto start = simulator.DecodeInstructions(simulator.start_instructions);
  if (start.empty()) return executed_steps;
  simulator.ApplyUpdates(*start[0]);
  executed_steps.push_back(start[0]->name);
  for (int step_count = 0;; step_count++) {
    auto step = simulator.DecodeInstructions(simulator.step_instructions);
    if (step.empty()) break;
    if (step_count == kMaxSteps)
      throw std::runtime_error("ILA: " + step[0]->name + " still decodes after " +
                               std::to_string(step_count) + " steps");
    simulator.ApplyUpdates(*step[0]);
    executed_steps.push_back(step[0]->name);
  }
  return executed_steps;
}

void IlaSim::WriteDram(uint64_t addr, uint8_t byte) {
  auto& simulator = *impl_;
  auto& dram = simulator.variable_values[simulator.dram_state];
  auto address = simulator.context.bv_val(addr, dram.get_sort().array_domain().bv_size());
  dram = z3::store(dram, address, simulator.context.bv_val(byte, DRAM_DATA_WIDTH));
}

ArchState IlaSim::State() const {
  const auto& simulator = *impl_;
  ArchState state;
  state.dram = simulator.ReadRows(simulator.dram_state, DRAM_DATA_WIDTH, Signedness::Unsigned);
  for (const auto& [index, row] :
       simulator.ReadRows(simulator.scratchpad_state, INPUT_TYPE_BIT_WIDTH, Signedness::Signed)) {
    if (index < simulator.shape.sp_rows) state.spad[index] = row;
    else state.stray.push_back("wrote scratchpad index " + Hex(index) + ", past the last row");
  }
  for (const auto& [address, row] :
       simulator.ReadRows(simulator.accumulator_state, ACC_TYPE_BIT_WIDTH, Signedness::Signed)) {
    auto row_index = DecodeAccumulatorRow(address, simulator.shape.acc_rows);
    if (row_index) state.acc[*row_index] = row;
    else state.stray.push_back("wrote accumulator index " + Hex(address) + ", not an accumulator row address");
  }
  return state;
}
