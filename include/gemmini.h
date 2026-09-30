#pragma once

#include <bits/stdc++.h>
#include <ilang/ilang++.h>

using namespace ilang;

namespace gemmini {

#define DRAM_ADDR_WIDTH 64
#define DRAM_DATA_WIDTH 32
#define GEMMINI_ADDR_WIDTH 32
#define RS_WIDTH 64

#define SYMB_TRUE BoolConst(true)
#define SYMB_FALSE BoolConst(false)
#define ALL_HIGH_BITS BvConst(4294967295, 32)

// TODO CHANGE LATER
// For now use 8-bit ints
#define INPUT_TYPE_BIT_WIDTH 8
#define ACC_TYPE_BIT_WIDTH 32

// RoCC funct7 values from the official ISA
#define FUNCT_WIDTH 7
#define config BvConst(0, FUNCT_WIDTH)
#define mvin2 BvConst(1, FUNCT_WIDTH)
#define mvin BvConst(2, FUNCT_WIDTH)
#define mvout BvConst(3, FUNCT_WIDTH)
#define matmul_compute_preloaded BvConst(4, FUNCT_WIDTH)
#define matmul_compute_accumulated BvConst(5, FUNCT_WIDTH)
#define matmul_preload BvConst(6, FUNCT_WIDTH)
#define mvin3 BvConst(14, FUNCT_WIDTH)

// mvin, mvin2 and mvin3 each have their own config_mvin registers
#define LOAD_PIPELINES 3

enum class DataType : uint64_t {
    INT8 = 8,
    INT16 = 16,
    INT32 = 32,
    FLOAT16 = 16,
    FLOAT32 = 32,
    // Add more as needed
};

// Configure Gemmini Paramaters
struct cfg {
    // Systolic array dimensions
    uint64_t tileRows;
    uint64_t tileColumns;
    uint64_t meshRows;
    uint64_t meshColumns;

    uint64_t DIM;

    // Scratchpad and accumulator memory
    uint64_t sp_banks;
    uint64_t sp_capacity;
    uint64_t acc_capacity;

    // Type paramaters
    DataType inputType;
    DataType outputType;
    DataType accType;

    cfg()
        : tileRows(16)
        , tileColumns(16)
        , meshRows(1)
        , meshColumns(1)
        , DIM(tileRows * meshRows)
        , sp_banks(4)
        , sp_capacity(64 * 1024)
        , // 64 KB
        acc_capacity(128 * 1024)
        , // 128 KB
        inputType(DataType::INT8)
        , outputType(DataType::INT16)
        , accType(DataType::INT32) { };

    cfg(
        uint64_t tileRows_,
        uint64_t tileColumns_,
        uint64_t meshRows_,
        uint64_t meshColumns_,
        uint64_t sp_banks_,
        uint64_t sp_capacity_,
        uint64_t acc_capacity_,
        DataType inputType_,
        DataType outputType_,
        DataType accType_)
        : tileRows(tileRows_)
        , tileColumns(tileColumns_)
        , meshRows(meshRows_)
        , meshColumns(meshColumns_)
        , DIM(tileRows * meshRows)
        , sp_banks(sp_banks_)
        , sp_capacity(sp_capacity_)
        , acc_capacity(acc_capacity_)
        , inputType(inputType_)
        , outputType(outputType_)
        , accType(accType_) { };
};

// A matrix operand of preload/compute: local address and size
struct MatrixOperand {
    ExprRef addr;
    ExprRef rows;
    ExprRef cols;
};

// Systolic array is made up of DIM x DIM PE
struct PE {
    ExprRef A_reg;
    ExprRef B_D_reg;
    ExprRef stationary_reg;
    ExprRef C_reg_out; // WS partial sum passed down
    ExprRef result_reg; // WS result C[row][col], held until the write-back cycle

    PE(Ila& m, size_t row, size_t col)
        : A_reg(m.NewBvState("PE_" + std::to_string(row) + "_" + std::to_string(col) + "_A", INPUT_TYPE_BIT_WIDTH))
        , B_D_reg(m.NewBvState("PE_" + std::to_string(row) + "_" + std::to_string(col) + "_B_D", INPUT_TYPE_BIT_WIDTH))
        , stationary_reg(m.NewBvState("PE_" + std::to_string(row) + "_" + std::to_string(col) + "_stat", ACC_TYPE_BIT_WIDTH))
        , C_reg_out(m.NewBvState("PE_" + std::to_string(row) + "_" + std::to_string(col) + "_C_out", ACC_TYPE_BIT_WIDTH))
        , result_reg(m.NewBvState("PE_" + std::to_string(row) + "_" + std::to_string(col) + "_result", ACC_TYPE_BIT_WIDTH)) { };
};

class Gemmini {

public:
    cfg _Cfg;

    Ila m;

    ExprRef dataflow;

    // Memory States
    ExprRef DRAM; 
    ExprRef scratchpad;
    ExprRef accumulator;
    std::vector<std::vector<std::unique_ptr<PE>>> sys_array;

    // Config states
    ExprRef shift;
    ExprRef A_stride;
    ExprRef memory_stride_mvout;
    ExprRef right_shift;
    ExprRef c_stride;

    ExprRef activation_func;
    ExprRef A_T;
    ExprRef B_T;
    ExprRef scalar;

    // Store config: accumulator read scale and activation
    ExprRef acc_scale;
    ExprRef acc_activation;

    // Load pipeline config, one entry per mvin/mvin2/mvin3
    std::vector<ExprRef> acc_type;
    std::vector<ExprRef> private_stride;
    std::vector<ExprRef> memory_stride_mvin;
    std::vector<ExprRef> scale;

    ExprRef max_pool_stride;
    ExprRef max_pool_window_size;
    ExprRef upper_zero_pad;
    ExprRef left_zero_pad;
    ExprRef out_dim;
    ExprRef pool_row;
    ExprRef pool_col;
    ExprRef unpool_row;
    ExprRef unpool_col;

    // mvin and mvout
    ExprRef mvin_DRAM_addr;
    ExprRef mvin_dest_addr;
    ExprRef mvin_col_num;
    ExprRef mvin_row_num;
    ExprRef mvin_destination;
    ExprRef mvin_pipeline;

    ExprRef mvout_DRAM_addr;
    ExprRef mvout_sour_addr;
    ExprRef mvout_col_num;
    ExprRef mvout_row_num;
    ExprRef mvout_source;

    ExprRef start_row;
    ExprRef start_chunk;
    ExprRef done;

    // matmul computation
    ExprRef preload_addr;
    ExprRef preload_row;
    ExprRef preload_col;
    ExprRef dest_addr;
    ExprRef dest_row;
    ExprRef dest_col;

    ExprRef A_addr;
    ExprRef A_row;
    ExprRef A_col;
    ExprRef B_D_addr;
    ExprRef B_D_row;
    ExprRef B_D_col;

    ExprRef cycle;
    ExprRef busy;

    // Decoding
    ExprRef funct;
    ExprRef rs1;
    ExprRef rs2;

    // Atomic computes matmul.compute.preloaded in one step instead of cycle by cycle
    enum class ComputeModel { Stepped, Atomic };
    ComputeModel compute_model;

    Gemmini(cfg Cfg, const std::string& name = "Gemmini", ComputeModel model = ComputeModel::Stepped);
    void AddInstructions();

    Ila& get()
    {
        return m;
    }

private:
    ExprRef SpadElem(const MatrixOperand& matrix, const ExprRef& row, const ExprRef& col);
    ExprRef ElemA(const MatrixOperand& A, const ExprRef& i, const ExprRef& k);
    ExprRef ElemB(const MatrixOperand& B, const ExprRef& k, const ExprRef& j);
    ExprRef ElemD(const MatrixOperand& D, const ExprRef& i, const ExprRef& j);
    ExprRef PreloadElem(size_t i, size_t j);
    void SetResultWrite(InstrRef& instr, const std::vector<std::vector<ExprRef>>& results, const ExprRef& write_now);
    void AddSteppedCompute(const std::string& name, const ExprRef& op, bool preload);
    void AddAtomicCompute();
};

// Helper Functions
inline ExprRef ResizeBv(const ExprRef& e, unsigned target_width)
{
    unsigned cur_width = e.bit_width();
    if (cur_width == target_width) {
        return e;
    } else if (cur_width < target_width) {
        return ZExt(e, target_width); // widening
    } else {
        return Extract(e, target_width - 1, 0); // narrowing (truncate high bits)
    }
}

inline uint64_t getBitWidth(DataType dataType)
{
    return static_cast<uint64_t>(dataType);
}

inline ExprRef Relu(const ExprRef& x)
{
    auto zero = BvConst(0, x.bit_width());
    auto is_neg = (x < zero);
    return Ite(is_neg, zero, x);
}

// Rounds v / 2^shift (arithmetic) to the nearest integer, ties to even
inline ExprRef RoundShiftRightEven(const ExprRef& v, const ExprRef& shift)
{
    auto width = v.bit_width();
    auto one = BvConst(1, width);
    auto quotient = v >> shift;
    auto half_bit = (v >> (shift - one)) & one;
    auto below_half = v & ((one << (shift - one)) - one);
    auto round_up = (half_bit == one) & ((below_half != BvConst(0, width)) | ((quotient & one) == one));
    return Ite(shift == BvConst(0, width), v, Ite(round_up, quotient + one, quotient));
}

// Rounds a non-negative integer below 2^value_bits to float32 precision: 24 significant bits,
// ties to even
inline ExprRef RoundToFloat32(const ExprRef& magnitude, unsigned value_bits)
{
    auto width = magnitude.bit_width();
    ExprRef excess_bits = BvConst(0, width);
    for (unsigned bit = 24; bit < value_bits; bit++)
        excess_bits = Ite(SelectBit(magnitude, bit) == BvConst(1, 1), BvConst(bit - 23, width), excess_bits);
    return RoundShiftRightEven(magnitude, excess_bits) << excess_bits;
}

// Official float scale: float32(x) * scale in float32, rounded to nearest even, saturated to out_width bits
inline ExprRef FloatScale(const ExprRef& x, const ExprRef& scale_bits, unsigned out_width)
{
    const unsigned W = 64;
    auto x_width = x.bit_width();
    auto x_neg = SelectBit(x, x_width - 1) == BvConst(1, 1);
    auto x_mag = ZExt(Ite(x_neg, -x, x), W);
    auto negative = x_neg != (SelectBit(scale_bits, 31) == BvConst(1, 1));
    auto exponent = ZExt(Extract(scale_bits, 30, 23), W);
    auto significand = ZExt(Concat(BvConst(1, 1), Extract(scale_bits, 22, 0)), W);

    // |x * scale| = product * 2^(exponent - 150)
    auto x_float = x_width > 24 ? RoundToFloat32(x_mag, x_width) : x_mag;
    auto product = RoundToFloat32(x_float * significand, x_width + 24);
    auto overflows = Uge(exponent, BvConst(150, W)) & (product != BvConst(0, W));
    auto magnitude = RoundShiftRightEven(product, BvConst(150, W) - exponent);

    auto max_mag = Ite(negative, BvConst(1ULL << (out_width - 1), W), BvConst((1ULL << (out_width - 1)) - 1, W));
    auto saturated = Ite(overflows | Ugt(magnitude, max_mag), max_mag, magnitude);
    auto result = Extract(saturated, out_width - 1, 0);
    return Ite(negative, -result, result);
}

inline ExprRef MvinScale(const ExprRef& x, const ExprRef& scale_bits)
{
    return FloatScale(x, scale_bits, x.bit_width());
}

// Saturates a signed value to a signed out_width-bit value
inline ExprRef Saturate(const ExprRef& x, unsigned out_width)
{
    auto width = x.bit_width();
    auto max = BvConst((1ULL << (out_width - 1)) - 1, width);
    auto min = -BvConst(1ULL << (out_width - 1), width);
    return Extract(Ite(x > max, max, Ite(x < min, min, x)), out_width - 1, 0);
}

} // namespace gemmini
