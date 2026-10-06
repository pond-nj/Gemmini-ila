#include "gemmini.h"

#include <bits/stdc++.h>
#include <ilang/ilang++.h>

namespace gemmini {

Gemmini::Gemmini(cfg Cfg, const std::string& name, ComputeModel model)
    : m(Ila(name))
    ,
    // ---------- Inputs ----------
    funct(m.NewBvInput("funct", FUNCT_WIDTH))
    , rs1(m.NewBvInput("rs1", RS_WIDTH))
    , rs2(m.NewBvInput("rs2", RS_WIDTH))
    ,
    // ---------- Config states ----------
    dataflow(m.NewBoolState("dataflow"))
    , shift(m.NewBvState("shift", 32))
    , A_stride(m.NewBvState("A_stride", 16))
    , memory_stride_mvout(m.NewBvState("memory_stride_mvout", 64))
    , right_shift(m.NewBvState("right_shift", 32))
    , c_stride(m.NewBvState("c_stride", 16))
    , activation_func(m.NewBvState("activation_func", 2))
    , A_T(m.NewBvState("A_T", 1))
    , B_T(m.NewBvState("B_T", 1))
    , scalar(m.NewBvState("scalar", 32))
    , acc_scale(m.NewBvState("acc_scale", 32))
    , acc_activation(m.NewBvState("acc_activation", 2))
    , max_pool_stride(m.NewBvState("max_pool_stride", 2))
    , max_pool_window_size(m.NewBvState("max_pool_window_size", 2))
    , upper_zero_pad(m.NewBvState("upper_zero_pad", 2))
    , left_zero_pad(m.NewBvState("left_zero_pad", 2))
    , out_dim(m.NewBvState("out_dim", 8))
    , pool_row(m.NewBvState("pool_row", 8))
    , pool_col(m.NewBvState("pool_col", 8))
    , unpool_row(m.NewBvState("unpool_row", 8))
    , unpool_col(m.NewBvState("unpool_col", 8))
    ,
    // ---------- Extra for mvin and mvout----
    mvin_DRAM_addr(m.NewBvState("mvin_DRAM_addr", 64))
    , mvin_dest_addr(m.NewBvState("mvin_dest_addr", 32))
    , mvin_col_num(m.NewBvState("mvin_col_num", 16))
    , mvin_row_num(m.NewBvState("mvin_row_num", 16))
    , mvin_destination(m.NewBvState("mvin_destination", 1))
    , mvin_pipeline(m.NewBvState("mvin_pipeline", 2))
    , mvout_DRAM_addr(m.NewBvState("mvout_DRAM_addr", 64))
    , mvout_sour_addr(m.NewBvState("mvout_sour_addr", 32))
    , mvout_col_num(m.NewBvState("mvout_col_num", 16))
    , mvout_row_num(m.NewBvState("mvout_row_num", 16))
    , mvout_source(m.NewBvState("mvout_source", 1))
    , start_row(m.NewBvState("start_row", 16))
    , start_chunk(m.NewBvState("start_chunk", 16))
    , done(m.NewBoolState("done"))
    ,
    // ---------- Extra for preload and computation ----
    preload_addr(m.NewBvState("preload_addr", 32))
    , preload_row(m.NewBvState("preload_row", 16))
    , preload_col(m.NewBvState("preload_col", 16))
    , dest_addr(m.NewBvState("dest_addr", 32))
    , dest_row(m.NewBvState("dest_row", 16))
    , dest_col(m.NewBvState("dest_col", 16))
    , A_addr(m.NewBvState("A_addr", 32))
    , A_row(m.NewBvState("A_row", 16))
    , A_col(m.NewBvState("A_col", 16))
    , B_D_addr(m.NewBvState("B_D_addr", 32))
    , B_D_row(m.NewBvState("B_D_row", 16))
    , B_D_col(m.NewBvState("B_D_col", 16))
    , cycle(m.NewBvState("cycle", 32))
    , busy(m.NewBoolState("busy"))
    // ---------- Memory states --------------
    , DRAM(m.NewMemState("DRAM", DRAM_ADDR_WIDTH, DRAM_DATA_WIDTH))
    , scratchpad(m.NewMemState("scratchpad", 32, Cfg.DIM * getBitWidth(Cfg.inputType)))
    , accumulator(m.NewMemState("accumulator", 32, Cfg.DIM * getBitWidth(Cfg.accType)))
{
    // ---------- Store config ----------
    _Cfg = Cfg;
    compute_model = model;
    auto DIM = Cfg.DIM;
    for (size_t p = 0; p < LOAD_PIPELINES; p++) {
        auto suffix = "_" + std::to_string(p);
        acc_type.push_back(m.NewBvState("acc_type" + suffix, 1));
        private_stride.push_back(m.NewBvState("private_stride" + suffix, 16));
        memory_stride_mvin.push_back(m.NewBvState("memory_stride_mvin" + suffix, 64));
        scale.push_back(m.NewBvState("scale" + suffix, 32));
    }
    // ---------- Create Systolic Array ----------
    sys_array.resize(DIM);
    for (size_t i = 0; i < DIM; i++) {
        sys_array[i].resize(DIM);
        for (size_t j = 0; j < DIM; j++) {
            sys_array[i][j] = std::make_unique<PE>(m, i, j);
        }
    }
};

void Gemmini::AddInstructions()
{
    const size_t DIM = _Cfg.DIM;
    const size_t INPUT_BITS = getBitWidth(_Cfg.inputType);
    const size_t ACC_BITS = getBitWidth(_Cfg.accType);

    {

        // Data Movement
        {

            // mvin, mvin2, mvin3 start
            {
                InstrRef instr = m.NewInstr("mvin");
                auto pipeline = Ite(funct == mvin2, BvConst(1, 2), Ite(funct == mvin3, BvConst(2, 2), BvConst(0, 2)));
                instr.SetDecode((funct == mvin) | (funct == mvin2) | (funct == mvin3));

                instr.SetUpdate(start_row, BvConst(0, 16));
                instr.SetUpdate(start_chunk, BvConst(0, 16));
                instr.SetUpdate(done, BoolConst(false));
                instr.SetUpdate(mvin_DRAM_addr, rs1);
                instr.SetUpdate(mvin_dest_addr, Extract(rs2, 31, 0));
                instr.SetUpdate(mvin_col_num, Extract(rs2, 47, 32));
                instr.SetUpdate(mvin_row_num, Extract(rs2, 63, 48));
                instr.SetUpdate(mvin_destination, Extract(rs2, 31, 31));
                instr.SetUpdate(mvin_pipeline, pipeline);
            }

            // mvin step: moves one row of one DIM-column block
            {
                InstrRef instr = m.NewInstr("mvin_step");
                instr.SetDecode(((funct == mvin) | (funct == mvin2) | (funct == mvin3)) & !done);

                auto SelectPipeline = [&](const std::vector<ExprRef>& per_pipeline) {
                    return Ite(mvin_pipeline == BvConst(1, 2), per_pipeline[1],
                        Ite(mvin_pipeline == BvConst(2, 2), per_pipeline[2], per_pipeline[0]));
                };
                auto dram_stride = SelectPipeline(memory_stride_mvin);
                auto block_stride = SelectPipeline(private_stride);
                auto mvin_scale = SelectPipeline(scale);
                auto shrunk = SelectPipeline(acc_type) == BvConst(1, 1);

                auto to_acc = mvin_destination == BvConst(1, 1);
                auto accumulate = Extract(mvin_dest_addr, 30, 30) == BvConst(1, 1);
                auto full_width = to_acc & !shrunk;
                auto load_zeros = mvin_DRAM_addr == BvConst(0, DRAM_ADDR_WIDTH);

                auto total_chunks = (mvin_col_num + BvConst(DIM - 1, 16)) / BvConst(DIM, 16);
                auto col_start = start_chunk * BvConst(DIM, 16);
                auto chunk_valid = Ult(col_start, mvin_col_num);

                // Bits [30:29] are flags, not part of the row address
                auto dest_row = Extract(mvin_dest_addr, 28, 0) + ZExt(start_row, 29) + ZExt(start_chunk, 29) * ZExt(block_stride, 29);
                auto dest_base = Concat(Extract(mvin_dest_addr, 31, 31), Concat(BvConst(0, 2), dest_row));

                auto elem_bytes = Ite(full_width, BvConst(ACC_BITS / 8, DRAM_ADDR_WIDTH), BvConst(INPUT_BITS / 8, DRAM_ADDR_WIDTH));
                auto dram_row = mvin_DRAM_addr + ZExt(start_row, DRAM_ADDR_WIDTH) * dram_stride;

                auto old_row_sp = scratchpad.Load(dest_base);
                auto old_row_acc = accumulator.Load(dest_base);
                ExprRef new_row_sp = BvConst(0, 1);
                ExprRef new_row_acc = BvConst(0, 1);

                for (size_t elem = 0; elem < DIM; elem++) {
                    auto col = ZExt(col_start, DRAM_ADDR_WIDTH) + BvConst(elem, DRAM_ADDR_WIDTH);
                    auto dram_elem_addr = dram_row + col * elem_bytes;

                    // DRAM is byte addressed; 32-bit elements are little-endian
                    ExprRef loaded = Extract(DRAM.Load(dram_elem_addr), 7, 0);
                    for (size_t byte = 1; byte < ACC_BITS / 8; byte++)
                        loaded = Concat(Extract(DRAM.Load(dram_elem_addr + BvConst(byte, DRAM_ADDR_WIDTH)), 7, 0), loaded);
                    loaded = Ite(load_zeros, BvConst(0, ACC_BITS), loaded);

                    auto scaled_input = MvinScale(Extract(loaded, INPUT_BITS - 1, 0), mvin_scale);
                    auto acc_value = Ite(full_width, loaded, SExt(scaled_input, ACC_BITS));

                    auto old_sp = Extract(old_row_sp, (elem + 1) * INPUT_BITS - 1, elem * INPUT_BITS);
                    auto old_acc = Extract(old_row_acc, (elem + 1) * ACC_BITS - 1, elem * ACC_BITS);
                    auto elem_valid = Ult(col, ZExt(mvin_col_num, DRAM_ADDR_WIDTH));

                    auto sp_elem = Ite(elem_valid, scaled_input, old_sp);
                    auto acc_elem = Ite(elem_valid, Ite(accumulate, old_acc + acc_value, acc_value), old_acc);

                    new_row_sp = (elem == 0) ? sp_elem : Concat(sp_elem, new_row_sp);
                    new_row_acc = (elem == 0) ? acc_elem : Concat(acc_elem, new_row_acc);
                }

                auto should_transfer = !done & chunk_valid;
                instr.SetUpdate(scratchpad, Ite(should_transfer & !to_acc, scratchpad.Store(dest_base, new_row_sp), scratchpad));
                instr.SetUpdate(accumulator, Ite(should_transfer & to_acc, accumulator.Store(dest_base, new_row_acc), accumulator));

                // Advance chunk counter
                auto next_chunk = start_chunk + BvConst(1, 16);
                auto chunk_overflow = Uge(next_chunk, total_chunks);
                auto new_chunk = Ite(chunk_overflow, BvConst(0, 16), next_chunk);
                auto new_row = Ite(chunk_overflow, start_row + BvConst(1, 16), start_row);

                instr.SetUpdate(start_chunk, Ite(!done, new_chunk, start_chunk));
                instr.SetUpdate(start_row, Ite(!done, new_row, start_row));

                // Set done when all chunks and rows are done
                auto all_rows_done = Uge(new_row, mvin_row_num);
                instr.SetUpdate(done, Ite(chunk_overflow & all_rows_done, BoolConst(true), done));
            }
        }

        {
            // mvout start
            {
                InstrRef instr = m.NewInstr("mvout");
                auto decode = mvout;
                instr.SetDecode(funct == decode);
                auto DRAM_addr = Extract(rs1, 63, 0);
                auto sour_addr = Extract(rs2, 31, 0);
                auto col_num = Extract(rs2, 47, 32);
                auto row_num = Extract(rs2, 63, 48);
                auto source = Extract(rs2, 31, 31);

                instr.SetUpdate(start_row, BvConst(0, 16));
                instr.SetUpdate(start_chunk, BvConst(0, 16));
                instr.SetUpdate(done, BoolConst(false));
                instr.SetUpdate(mvout_DRAM_addr, DRAM_addr);
                instr.SetUpdate(mvout_sour_addr, sour_addr);
                instr.SetUpdate(mvout_row_num, row_num);
                instr.SetUpdate(mvout_col_num, col_num);
                instr.SetUpdate(mvout_source, source);
            }

            // mvout steps: each moves one row of one DIM-column block. Reads from the scratchpad
            // and from the accumulator are separate instructions.
            for (bool from_acc : { false, true }) {
                InstrRef instr = m.NewInstr(from_acc ? "mvout_acc_step" : "mvout_step");
                instr.SetDecode((funct == mvout) & !done & (mvout_source == BvConst(from_acc, 1)));

                auto total_chunks = (mvout_col_num + BvConst(DIM - 1, 16)) / BvConst(DIM, 16);
                auto col_start = start_chunk * BvConst(DIM, 16);
                auto should_transfer = Ult(col_start, mvout_col_num) & Ult(start_row, mvout_row_num);

                // Bits [30:26] are flags and the norm command, not part of the row address
                auto source_row = Extract(mvout_sour_addr, 25, 0) + ZExt(start_row, 26) + ZExt(start_chunk, 26) * BvConst(DIM, 26);
                auto source_base = Concat(Extract(mvout_sour_addr, 31, 31), Concat(BvConst(0, 5), source_row));
                auto source_data = from_acc ? accumulator.Load(source_base) : scratchpad.Load(source_base);
                auto dram_row = mvout_DRAM_addr + ZExt(start_row, DRAM_ADDR_WIDTH) * memory_stride_mvout;

                // DRAM is byte addressed. Each element is one byte, or four little-endian bytes
                // for a full-width (bit 29) accumulator read, which is not scaled or activated.
                ExprRef dram_bytes = DRAM;
                ExprRef dram_full = DRAM;
                for (size_t elem = 0; elem < DIM; elem++) {
                    auto col = ZExt(col_start, DRAM_ADDR_WIDTH) + BvConst(elem, DRAM_ADDR_WIDTH);
                    auto elem_valid = should_transfer & Ult(col, ZExt(mvout_col_num, DRAM_ADDR_WIDTH));

                    if (from_acc) {
                        auto value = Extract(source_data, (elem + 1) * ACC_BITS - 1, elem * ACC_BITS);
                        auto scaled = FloatScale(value, acc_scale, INPUT_BITS);
                        auto activated = Ite(acc_activation == BvConst(1, 2), Relu(scaled), scaled);
                        dram_bytes = Ite(elem_valid, dram_bytes.Store(dram_row + col, ZExt(activated, DRAM_DATA_WIDTH)), dram_bytes);

                        ExprRef with_elem = dram_full;
                        for (size_t byte = 0; byte < ACC_BITS / 8; byte++) {
                            auto addr = dram_row + col * BvConst(ACC_BITS / 8, DRAM_ADDR_WIDTH) + BvConst(byte, DRAM_ADDR_WIDTH);
                            with_elem = with_elem.Store(addr, ZExt(Extract(value, 8 * byte + 7, 8 * byte), DRAM_DATA_WIDTH));
                        }
                        dram_full = Ite(elem_valid, with_elem, dram_full);
                    } else {
                        auto value = Extract(source_data, (elem + 1) * INPUT_BITS - 1, elem * INPUT_BITS);
                        dram_bytes = Ite(elem_valid, dram_bytes.Store(dram_row + col, ZExt(value, DRAM_DATA_WIDTH)), dram_bytes);
                    }
                }
                auto full_width = Extract(mvout_sour_addr, 29, 29) == BvConst(1, 1);
                instr.SetUpdate(DRAM, from_acc ? Ite(full_width, dram_full, dram_bytes) : dram_bytes);

                auto next_chunk = start_chunk + BvConst(1, 16);
                auto chunk_overflow = Uge(next_chunk, total_chunks);
                auto new_row = Ite(chunk_overflow, start_row + BvConst(1, 16), start_row);
                instr.SetUpdate(start_chunk, Ite(chunk_overflow, BvConst(0, 16), next_chunk));
                instr.SetUpdate(start_row, new_row);
                instr.SetUpdate(done, chunk_overflow & Uge(new_row, mvout_row_num));
            }
        }
    }

    {
        // Configuration
        {
            // config_ex
            InstrRef instr = m.NewInstr("config_ex");
            auto decode = config;
            auto type = Extract(rs1, 1, 0);
            instr.SetDecode((funct == decode) & (type == BvConst(0, 2)));

            auto set_only_strides = Extract(rs1, 7, 7) == BvConst(1, 1);
            auto SetUnlessOnlyStrides = [&](const ExprRef& state, const ExprRef& value) {
                instr.SetUpdate(state, Ite(set_only_strides, state, value));
            };
            SetUnlessOnlyStrides(dataflow, Extract(rs1, 2, 2) == BvConst(1, 1));
            SetUnlessOnlyStrides(activation_func, Extract(rs1, 4, 3));
            SetUnlessOnlyStrides(A_T, Extract(rs1, 8, 8));
            SetUnlessOnlyStrides(B_T, Extract(rs1, 9, 9));
            SetUnlessOnlyStrides(scalar, Extract(rs1, 63, 32));
            SetUnlessOnlyStrides(right_shift, Extract(rs2, 31, 0));
            instr.SetUpdate(A_stride, Extract(rs1, 31, 16));
            instr.SetUpdate(c_stride, Extract(rs2, 63, 48));
        }

        {
            // config_mvin
            InstrRef instr = m.NewInstr("config_mvin");
            auto decode = config;
            auto type = Extract(rs1, 1, 0);
            instr.SetDecode((funct == decode) & (type == BvConst(1, 2)));
            auto pipeline = Extract(rs1, 4, 3);
            for (size_t p = 0; p < LOAD_PIPELINES; p++) {
                auto selected = pipeline == BvConst(p, 2);
                instr.SetUpdate(acc_type[p], Ite(selected, Extract(rs1, 2, 2), acc_type[p]));
                instr.SetUpdate(private_stride[p], Ite(selected, Extract(rs1, 31, 16), private_stride[p]));
                instr.SetUpdate(memory_stride_mvin[p], Ite(selected, rs2, memory_stride_mvin[p]));
                instr.SetUpdate(scale[p], Ite(selected, Extract(rs1, 63, 32), scale[p]));
            }
        }

        {
            // config_mvout
            InstrRef instr = m.NewInstr("config_mvout");
            auto decode = config;
            auto type = Extract(rs1, 1, 0);
            instr.SetDecode((funct == decode) & (type == BvConst(2, 2)));
            instr.SetUpdate(acc_activation, Extract(rs1, 3, 2));
            instr.SetUpdate(max_pool_stride, Extract(rs1, 5, 4));
            instr.SetUpdate(max_pool_window_size, Extract(rs1, 7, 6));
            instr.SetUpdate(upper_zero_pad, Extract(rs1, 9, 8));
            instr.SetUpdate(left_zero_pad, Extract(rs1, 11, 10));
            instr.SetUpdate(out_dim, Extract(rs1, 31, 24));
            instr.SetUpdate(pool_row, Extract(rs1, 39, 32));
            instr.SetUpdate(pool_col, Extract(rs1, 47, 40));
            instr.SetUpdate(unpool_row, Extract(rs1, 55, 48));
            instr.SetUpdate(unpool_col, Extract(rs1, 63, 56));
            instr.SetUpdate(memory_stride_mvout, ZExt(Extract(rs2, 31, 0), 64));
            instr.SetUpdate(acc_scale, Extract(rs2, 63, 32));
        }
    }

    {
        // Core matmul sequence
        {
            // matmul.preload: records the D/B and C operands; the PEs are loaded by matmul.compute.preloaded
            InstrRef instr = m.NewInstr("matmul.preload");
            instr.SetDecode(funct == matmul_preload);
            instr.SetUpdate(preload_addr, Extract(rs1, 31, 0));
            instr.SetUpdate(preload_col, Extract(rs1, 47, 32));
            instr.SetUpdate(preload_row, Extract(rs1, 63, 48));
            instr.SetUpdate(dest_addr, Extract(rs2, 31, 0));
            instr.SetUpdate(dest_col, Extract(rs2, 47, 32));
            instr.SetUpdate(dest_row, Extract(rs2, 63, 48));
        }

        if (compute_model == ComputeModel::Atomic)
            AddAtomicCompute();
        else
            AddSteppedCompute("matmul.compute.preloaded", matmul_compute_preloaded, true);
        AddSteppedCompute("matmul.compute.accumulated", matmul_compute_accumulated, false);
    }
}

// Element (row, col) of a scratchpad matrix; the all-high-bits address reads zeros
ExprRef Gemmini::SpadElem(const MatrixOperand& matrix, const ExprRef& row, const ExprRef& col)
{
    const size_t INPUT_BITS = getBitWidth(_Cfg.inputType);
    auto row_data = scratchpad.Load(matrix.addr + row);
    auto bit_offset = ResizeBv(col * BvConst(INPUT_BITS, 32), row_data.bit_width());
    auto elem = Extract(Lshr(row_data, bit_offset), INPUT_BITS - 1, 0);
    return Ite(matrix.addr == ALL_HIGH_BITS, BvConst(0, INPUT_BITS), elem);
}

// A[i][k], zero outside the operand's rows x cols
ExprRef Gemmini::ElemA(const MatrixOperand& A, const ExprRef& i, const ExprRef& k)
{
    auto transposed = A_T == BvConst(1, 1);
    auto row = ZExt(A_stride, 32) * Ite(transposed, k, i);
    auto col = Ite(transposed, i, k);
    auto in_bounds = Ult(i, ZExt(A.rows, 32)) & Ult(k, ZExt(A.cols, 32));
    return Ite(in_bounds, SpadElem(A, row, col), BvConst(0, getBitWidth(_Cfg.inputType)));
}

// B[k][j] (output-stationary), zero outside the operand's rows x cols
ExprRef Gemmini::ElemB(const MatrixOperand& B, const ExprRef& k, const ExprRef& j)
{
    auto transposed = B_T == BvConst(1, 1);
    auto in_bounds = Ult(k, ZExt(B.rows, 32)) & Ult(j, ZExt(B.cols, 32));
    auto elem = SpadElem(B, Ite(transposed, j, k), Ite(transposed, k, j));
    return Ite(in_bounds, elem, BvConst(0, getBitWidth(_Cfg.inputType)));
}

// D[i][j] (weight-stationary bias), zero outside the operand's rows x cols
ExprRef Gemmini::ElemD(const MatrixOperand& D, const ExprRef& i, const ExprRef& j)
{
    auto in_bounds = Ult(i, ZExt(D.rows, 32)) & Ult(j, ZExt(D.cols, 32));
    return Ite(in_bounds, SpadElem(D, i, j), BvConst(0, getBitWidth(_Cfg.inputType)));
}

// Value preloaded into PE (i, j): D[i][j] (OS) or B[i][j] (WS, transposed if B_T)
ExprRef Gemmini::PreloadElem(size_t i, size_t j)
{
    MatrixOperand preloaded { preload_addr, preload_row, preload_col };
    auto transposed = dataflow & (B_T == BvConst(1, 1));
    auto row = Ite(transposed, BvConst(j, 32), BvConst(i, 32));
    auto col = Ite(transposed, BvConst(i, 32), BvConst(j, 32));
    auto in_bounds = Ult(BvConst(i, 16), preload_row) & Ult(BvConst(j, 16), preload_col);
    auto elem = Ite(in_bounds, SpadElem(preloaded, row, col), BvConst(0, getBitWidth(_Cfg.inputType)));
    return SExt(elem, getBitWidth(_Cfg.accType));
}

// Writes results[i][j] to the C operand of matmul.preload, row i at C + c_stride * i.
// The accumulator gets the raw value (bit 30: added to it). The scratchpad gets it rounding
// right-shifted (OS only), saturated and, if configured, passed through ReLU.
void Gemmini::SetResultWrite(InstrRef& instr, const std::vector<std::vector<ExprRef>>& results, const ExprRef& write_now)
{
    const size_t DIM = _Cfg.DIM;
    const size_t INPUT_BITS = getBitWidth(_Cfg.inputType);
    const size_t ACC_BITS = getBitWidth(_Cfg.accType);

    auto to_acc = Extract(dest_addr, 31, 31) == BvConst(1, 1);
    auto accumulate = Extract(dest_addr, 30, 30) == BvConst(1, 1);
    auto shift = Ite(dataflow, BvConst(0, 32), right_shift);
    auto relu = activation_func == BvConst(1, 2);

    ExprRef new_sp = scratchpad;
    ExprRef new_acc = accumulator;
    for (size_t i = 0; i < DIM; i++) {
        auto row = Extract(dest_addr, 28, 0) + ZExt(c_stride, 29) * BvConst(i, 29);
        auto index = Concat(Extract(dest_addr, 31, 31), Concat(BvConst(0, 2), row));
        auto old_sp = new_sp.Load(index);
        auto old_acc = new_acc.Load(index);
        ExprRef row_sp = BvConst(0, 1);
        ExprRef row_acc = BvConst(0, 1);
        for (size_t j = 0; j < DIM; j++) {
            auto valid = Ult(BvConst(j, 16), dest_col);
            auto old_sp_elem = Extract(old_sp, (j + 1) * INPUT_BITS - 1, j * INPUT_BITS);
            auto old_acc_elem = Extract(old_acc, (j + 1) * ACC_BITS - 1, j * ACC_BITS);
            auto shifted = Saturate(RoundShiftRightEven(results[i][j], shift), INPUT_BITS);
            auto sp_elem = Ite(valid, Ite(relu, Relu(shifted), shifted), old_sp_elem);
            auto acc_elem = Ite(valid, Ite(accumulate, old_acc_elem + results[i][j], results[i][j]), old_acc_elem);
            row_sp = j == 0 ? sp_elem : Concat(sp_elem, row_sp);
            row_acc = j == 0 ? acc_elem : Concat(acc_elem, row_acc);
        }
        auto row_valid = Ult(BvConst(i, 16), dest_row);
        new_sp = Ite(row_valid & !to_acc, new_sp.Store(index, row_sp), new_sp);
        new_acc = Ite(row_valid & to_acc, new_acc.Store(index, row_acc), new_acc);
    }
    auto write = write_now & (dest_addr != ALL_HIGH_BITS);
    instr.SetUpdate(scratchpad, Ite(write, new_sp, scratchpad));
    instr.SetUpdate(accumulator, Ite(write, new_acc, accumulator));
}

// Cycle-by-cycle compute on the systolic array. A enters from the left and moves right. In OS,
// B enters from the top and moves down, and PE (r, c) accumulates A[r][k] * B[k][c] with
// k = cycle - r - c. In WS, PE (r, c) holds B[r][c] and partial sums of output row
// i = cycle - r - c move down, starting from D. The last PE finishes at cycle 3 * DIM - 3,
// and cycle 3 * DIM - 2 writes C.
void Gemmini::AddSteppedCompute(const std::string& name, const ExprRef& op, bool preload)
{
    const size_t DIM = _Cfg.DIM;
    const size_t INPUT_BITS = getBitWidth(_Cfg.inputType);
    const size_t ACC_BITS = getBitWidth(_Cfg.accType);

    {
        InstrRef instr = m.NewInstr(name);
        instr.SetDecode((funct == op) & !busy);
        instr.SetUpdate(A_addr, Extract(rs1, 31, 0));
        instr.SetUpdate(A_col, Extract(rs1, 47, 32));
        instr.SetUpdate(A_row, Extract(rs1, 63, 48));
        instr.SetUpdate(B_D_addr, Extract(rs2, 31, 0));
        instr.SetUpdate(B_D_col, Extract(rs2, 47, 32));
        instr.SetUpdate(B_D_row, Extract(rs2, 63, 48));
        instr.SetUpdate(cycle, BvConst(0, 32));
        instr.SetUpdate(busy, SYMB_TRUE);
        for (size_t r = 0; r < DIM; r++) {
            for (size_t c = 0; c < DIM; c++) {
                auto& pe = *sys_array[r][c];
                instr.SetUpdate(pe.A_reg, BvConst(0, INPUT_BITS));
                instr.SetUpdate(pe.B_D_reg, BvConst(0, INPUT_BITS));
                instr.SetUpdate(pe.C_reg_out, BvConst(0, ACC_BITS));
                if (preload)
                    instr.SetUpdate(pe.stationary_reg, PreloadElem(r, c));
            }
        }
    }

    {
        InstrRef instr = m.NewInstr(name + "_step");
        instr.SetDecode((funct == op) & busy);
        MatrixOperand A { A_addr, A_row, A_col };
        MatrixOperand BD { B_D_addr, B_D_row, B_D_col };

        std::vector<ExprRef> bottom_psum;
        std::vector<std::vector<ExprRef>> results(DIM);
        for (size_t r = 0; r < DIM; r++) {
            for (size_t c = 0; c < DIM; c++) {
                auto& pe = *sys_array[r][c];
                auto skewed = cycle - BvConst(r + c, 32);
                auto pe_row = BvConst(r, 32);
                auto pe_col = BvConst(c, 32);

                auto a_in = c == 0 ? Ite(dataflow, ElemA(A, skewed, pe_row), ElemA(A, pe_row, skewed)) : sys_array[r][c - 1]->A_reg;
                auto b_in = r == 0 ? ElemB(BD, skewed, pe_col) : sys_array[r - 1][c]->B_D_reg;
                auto psum_in = r == 0 ? SExt(ElemD(BD, skewed, pe_col), ACC_BITS) : sys_array[r - 1][c]->C_reg_out;
                auto a = SExt(a_in, ACC_BITS);
                auto psum_out = psum_in + a * pe.stationary_reg;

                instr.SetUpdate(pe.A_reg, a_in);
                instr.SetUpdate(pe.B_D_reg, b_in);
                instr.SetUpdate(pe.C_reg_out, psum_out);
                instr.SetUpdate(pe.stationary_reg, Ite(dataflow, pe.stationary_reg, pe.stationary_reg + a * SExt(b_in, ACC_BITS)));
                results[r].push_back(Ite(dataflow, pe.result_reg, pe.stationary_reg));
                if (r == DIM - 1)
                    bottom_psum.push_back(psum_out);
            }
        }

        // WS: C[i][c] leaves the bottom row at cycle i + DIM - 1 + c
        for (size_t i = 0; i < DIM; i++) {
            for (size_t c = 0; c < DIM; c++) {
                auto& result = sys_array[i][c]->result_reg;
                instr.SetUpdate(result, Ite(cycle == BvConst(i + DIM - 1 + c, 32), bottom_psum[c], result));
            }
        }

        auto write_cycle = cycle == BvConst(3 * DIM - 2, 32);
        SetResultWrite(instr, results, write_cycle);
        instr.SetUpdate(busy, !write_cycle);
        instr.SetUpdate(cycle, cycle + BvConst(1, 32));
    }
}

// matmul.compute.preloaded in one step
void Gemmini::AddAtomicCompute()
{
    const size_t DIM = _Cfg.DIM;
    const size_t ACC_BITS = getBitWidth(_Cfg.accType);

    InstrRef instr = m.NewInstr("matmul.compute.atomic");
    instr.SetDecode(funct == matmul_compute_preloaded);
    MatrixOperand A { Extract(rs1, 31, 0), Extract(rs1, 63, 48), Extract(rs1, 47, 32) };
    MatrixOperand BD { Extract(rs2, 31, 0), Extract(rs2, 63, 48), Extract(rs2, 47, 32) };

    std::vector<std::vector<ExprRef>> preloaded(DIM);
    for (size_t i = 0; i < DIM; i++)
        for (size_t j = 0; j < DIM; j++)
            preloaded[i].push_back(PreloadElem(i, j));

    std::vector<std::vector<ExprRef>> results(DIM);
    for (size_t i = 0; i < DIM; i++) {
        for (size_t j = 0; j < DIM; j++) {
            auto row = BvConst(i, 32);
            auto col = BvConst(j, 32);
            ExprRef os_sum = preloaded[i][j];
            ExprRef ws_sum = SExt(ElemD(BD, row, col), ACC_BITS);
            for (size_t k = 0; k < DIM; k++) {
                auto a = SExt(ElemA(A, row, BvConst(k, 32)), ACC_BITS);
                os_sum = os_sum + a * SExt(ElemB(BD, BvConst(k, 32), col), ACC_BITS);
                ws_sum = ws_sum + a * preloaded[k][j];
            }
            results[i].push_back(Ite(dataflow, ws_sum, os_sum));
            instr.SetUpdate(sys_array[i][j]->stationary_reg, Ite(dataflow, preloaded[i][j], os_sum));
        }
    }
    SetResultWrite(instr, results, SYMB_TRUE);
}

}
