#include "gemmini.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <ilang/ilang++.h>

namespace gemmini {

// A matrix operand of preload/compute: local address and size
struct MatrixOperand {
    ExprRef addr;
    ExprRef rows;
    ExprRef cols;
};

Gemmini::Gemmini(cfg Cfg, const std::string& name, ComputeModel model)
    : m(Ila(name))
    ,
    // ---------- Inputs ----------
    funct(m.NewBvInput("funct", FUNCT_WIDTH)) // funct7: gemmini/src/main/scala/gemmini/GemminiISA.scala:60 (126.U(7.W))
    , rs1(m.NewBvInput("rs1", RS_WIDTH))
    , rs2(m.NewBvInput("rs2", RS_WIDTH))
    ,
    // ---------- Config states ----------
    dataflow(m.NewBoolState("dataflow"))
    , shift(m.NewBvState("shift", 32))
    , A_stride(m.NewBvState("A_stride", 16))
    , memory_stride_mvout(m.NewBvState("memory_stride_mvout", 64))
    , right_shift(m.NewBvState("right_shift", 32))
    , c_stride(m.NewBvState("c_stride", 16)) // config_ex rs2[63:48]: gemmini/src/main/scala/gemmini/GemminiISA.scala:221-225
    , activation_func(m.NewBvState("activation_func", 2)) // 2 bits: gemmini/src/main/scala/gemmini/GemminiISA.scala:194 (CONFIG_EX_RS1_ACTIVATION_WIDTH = 2)
    , A_T(m.NewBvState("A_T", 1))
    , B_T(m.NewBvState("B_T", 1))
    , scalar(m.NewBvState("scalar", 32))
    , acc_scale(m.NewBvState("acc_scale", 32)) // config_mvout rs2[63:32]: gemmini/src/main/scala/gemmini/GemminiISA.scala:155-160
    , acc_activation(m.NewBvState("acc_activation", 2)) // config_mvout rs1[3:2]: gemmini/src/main/scala/gemmini/GemminiISA.scala:137-150
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
    // D/B operand of matmul.preload rs1, used by the next compute: gemmini/src/main/scala/gemmini/ExecuteController.scala:84-87, 152-158
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
    // One config set per load state: gemmini/src/main/scala/gemmini/LoadController.scala:33-37
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
    const size_t INPUT_ROW_BITS = DIM * INPUT_BITS;
    const size_t ACC_ROW_BITS = DIM * ACC_BITS;

    {

        // Data Movement
        {

            // mvin, mvin2, mvin3 start
            // mvin2/mvin3 are mvin with their own config: gemmini/README.md:411-412; gemmini/src/main/scala/gemmini/LoadController.scala:61-62
            auto decode_mvin = (funct == mvin) | (funct == mvin2) | (funct == mvin3);
            {
                InstrRef instr = m.NewInstr("mvin");
                instr.SetDecode(decode_mvin);
                ExprRef DRAM_addr = Extract(rs1, 63, 0);
                ExprRef dest_addr = Extract(rs2, 31, 0);
                ExprRef col_num = Extract(rs2, 47, 32);
                ExprRef row_num = Extract(rs2, 63, 48);
                ExprRef destination = Extract(rs2, 31, 31);

                instr.SetUpdate(start_row, BvConst(0, 16));
                instr.SetUpdate(start_chunk, BvConst(0, 16));
                instr.SetUpdate(done, BoolConst(false));
                instr.SetUpdate(mvin_DRAM_addr, DRAM_addr);
                instr.SetUpdate(mvin_dest_addr, dest_addr);
                instr.SetUpdate(mvin_row_num, row_num);
                instr.SetUpdate(mvin_col_num, col_num);
                instr.SetUpdate(mvin_destination, destination);
            }

            // mvin step
            {
                InstrRef instr = m.NewInstr("mvin_step");
                instr.SetDecode(decode_mvin & !done);

                // Config of the load pipeline of this mvin
                // funct picks the load state: gemmini/src/main/scala/gemmini/LoadController.scala:61-70
                auto SelectPipeline = [&](const std::vector<ExprRef>& per_pipeline) {
                    return Ite(funct == mvin2, per_pipeline[1], Ite(funct == mvin3, per_pipeline[2], per_pipeline[0]));
                };
                auto memory_stride = SelectPipeline(memory_stride_mvin);
                auto block_stride = SelectPipeline(private_stride);
                auto mvin_scale = SelectPipeline(scale);
                auto shrunk = SelectPipeline(acc_type) == BvConst(1, 1);

                // Local address bit 30 = accumulate: gemmini/README.md:371; gemmini/src/main/scala/gemmini/LocalAddr.scala:22, Scratchpad.scala:828-832
                auto accumulate = Extract(mvin_dest_addr, 30, 30) == BvConst(1, 1);
                // Accumulator mvins read accType unless shrunk: gemmini/README.md:466; gemmini/src/main/scala/gemmini/LoadController.scala:108
                auto full_width = (mvin_destination == BvConst(1, 1)) & !shrunk;
                // DRAM address 0 writes zeros: gemmini/src/main/scala/gemmini/LoadController.scala:72,109; Scratchpad.scala:360-370 (ZeroWriter)
                auto load_zeros = mvin_DRAM_addr == BvConst(0, DRAM_ADDR_WIDTH);

                // Total chunks per row
                auto total_chunks = (mvin_col_num + BvConst(DIM - 1, 16)) / BvConst(DIM, 16);

                // Determine how many columns to process per chunk
                auto col_start = start_chunk * BvConst(DIM, 16);
                auto chunk_end = Ite((col_start + BvConst(DIM, 16)) <= mvin_col_num,
                    col_start + BvConst(DIM, 16),
                    mvin_col_num);
                auto chunk_width = chunk_end - col_start;

                auto chunk_valid = Ite(Ult(col_start, mvin_col_num), SYMB_TRUE, SYMB_FALSE);
                auto should_transfer = Ite(!done & chunk_valid, SYMB_TRUE, SYMB_FALSE);

                // Bytes per element: gemmini/src/main/scala/gemmini/LoadController.scala:115-117. DRAM row = vaddr + row * stride: LoadController.scala:102
                auto elem_bytes = Ite(full_width, BvConst(ACC_BITS / 8, DRAM_ADDR_WIDTH), BvConst(INPUT_BITS / 8, DRAM_ADDR_WIDTH));
                auto dram_base = mvin_DRAM_addr + (ZExt(start_row, DRAM_ADDR_WIDTH) * memory_stride) + (ZExt(col_start, DRAM_ADDR_WIDTH) * elem_bytes);
                // Flags stripped, row = low bits: gemmini/README.md:363-373; gemmini/src/main/scala/gemmini/LocalAddr.scala:21-36, Scratchpad.scala:835-837
                // Block c of a wide mvin goes to addr + c * block_stride: gemmini/src/main/scala/gemmini/DMA.scala:267-273
                // Bits [30:29] of the local address are flags, not part of the row address
                auto dest_row = Extract(mvin_dest_addr, 28, 0) + (ZExt(start_row, 29)) + (ZExt(start_chunk, 29) * ZExt(block_stride, 29));
                auto dest_base = Concat(Extract(mvin_dest_addr, 31, 31), Concat(BvConst(0, 2), dest_row));

                auto current_row_input = scratchpad.Load(dest_base);
                auto current_row_acc = accumulator.Load(dest_base);
                ExprRef new_row_input_concat = BvConst(0, INPUT_ROW_BITS);
                ExprRef new_row_acc_concat = BvConst(0, ACC_ROW_BITS);

                // Build the row
                for (size_t elem = 0; elem < DIM; elem++) {
                    auto elem_valid = Ite(BvConst(elem, 16) < chunk_width, SYMB_TRUE, SYMB_FALSE);
                    auto should_update = Ite(should_transfer & elem_valid, SYMB_TRUE, SYMB_FALSE);

                    // A row is read as Vec(cols, accType), element 0 in the low bytes: gemmini/src/main/scala/gemmini/Scratchpad.scala:846-848
                    // DRAM is byte addressed; a full-width element is 4 little-endian bytes
                    auto dram_elem_addr = dram_base + BvConst(elem, DRAM_ADDR_WIDTH) * elem_bytes;
                    ExprRef load_elem = Extract(DRAM.Load(dram_elem_addr), 7, 0);
                    for (size_t byte = 1; byte < ACC_BITS / 8; byte++)
                        load_elem = Concat(Extract(DRAM.Load(dram_elem_addr + BvConst(byte, DRAM_ADDR_WIDTH)), 7, 0), load_elem);
                    load_elem = Ite(load_zeros, BvConst(0, ACC_BITS), load_elem);

                    // Scale applied to non-full-width mvins only: gemmini/README.md:469; gemmini/src/main/scala/gemmini/Scratchpad.scala:407-428, 443-446;
                    //   mvin_scale_acc_args = None, so full-width accumulator mvins are not scaled: gemmini/src/main/scala/gemmini/Configs.scala:114
                    // Shrunk accumulator mvin sign-extends each element: gemmini/src/main/scala/gemmini/Scratchpad.scala:847, Arithmetic.scala:112-119
                    auto load_elem_input = MvinScale(Extract(load_elem, INPUT_BITS - 1, 0), mvin_scale);
                    auto load_elem_acc = Ite(full_width, load_elem, SExt(load_elem_input, ACC_BITS));

                    auto current_elem_input = Extract(current_row_input, (elem + 1) * INPUT_BITS - 1, elem * INPUT_BITS);
                    auto current_elem_acc = Extract(current_row_acc, (elem + 1) * ACC_BITS - 1, elem * ACC_BITS);
                    // bit 30 accumulates on write: gemmini/src/main/scala/gemmini/Scratchpad.scala:828-832
                    load_elem_acc = Ite(accumulate, current_elem_acc + load_elem_acc, load_elem_acc);

                    // Columns past num_cols are masked, not overwritten: gemmini/src/main/scala/gemmini/Scratchpad.scala:849-851
                    auto new_elem_input = Ite(should_update, load_elem_input, current_elem_input);
                    auto new_elem_acc = Ite(should_update, load_elem_acc, current_elem_acc);

                    if (elem == 0) {
                        new_row_input_concat = new_elem_input;
                        new_row_acc_concat = new_elem_acc;
                    } else {
                        new_row_input_concat = Concat(new_elem_input, new_row_input_concat);
                        new_row_acc_concat = Concat(new_elem_acc, new_row_acc_concat);
                    }
                }

                // Decide whether to update or not
                auto store_sp = scratchpad.Store(dest_base, new_row_input_concat);
                auto store_acc = accumulator.Store(dest_base, new_row_acc_concat);
                instr.SetUpdate(scratchpad, Ite(should_transfer & (mvin_destination == BvConst(0, 1)), store_sp, scratchpad));
                instr.SetUpdate(accumulator, Ite(should_transfer & (mvin_destination == BvConst(1, 1)), store_acc, accumulator));

                // Advance chunk counter
                auto next_chunk = start_chunk + BvConst(1, 16);
                auto chunk_overflow = Ite(Uge(next_chunk, total_chunks), SYMB_TRUE, SYMB_FALSE);
                auto new_chunk = Ite(chunk_overflow, BvConst(0, 16), next_chunk);
                auto new_row = Ite(chunk_overflow, start_row + BvConst(1, 16), start_row);

                instr.SetUpdate(start_chunk, Ite(!done, new_chunk, start_chunk));
                instr.SetUpdate(start_row, Ite(!done, new_row, start_row));

                // Set done when all chunks and rows are done
                auto all_rows_done = Uge(new_row, mvin_row_num);
                auto done_after = Ite(chunk_overflow & all_rows_done, SYMB_TRUE, SYMB_FALSE);
                instr.SetUpdate(done, Ite(done_after, BoolConst(true), done));
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

            // mvout step: moves one row of one DIM-column block
            {
                InstrRef instr = m.NewInstr("mvout_step");
                auto decode = mvout;
                instr.SetDecode((funct == decode) & !done);

                // Wide mvouts are split into DIM-column blocks: gemmini/src/main/scala/gemmini/StoreController.scala:95, 181, 216-217
                // Total chunks per row
                auto total_chunks = (mvout_col_num + BvConst(DIM - 1, 16)) / BvConst(DIM, 16);
                auto col_start = start_chunk * BvConst(DIM, 16);

                // Check if need to continue or not
                auto continue_row = Ite(start_row < mvout_row_num, SYMB_TRUE, SYMB_FALSE);

                // Get base address for each row
                auto dram_base = mvout_DRAM_addr + (ZExt(start_row, DRAM_ADDR_WIDTH) * memory_stride_mvout);
                // Local address layout (is_acc, accumulate, read_full, norm_cmd, data): gemmini/src/main/scala/gemmini/LocalAddr.scala:21-31, NormCmd.scala:8
                // Block c is read from addr + c * DIM rows: gemmini/src/main/scala/gemmini/StoreController.scala:41, 128
                // Bits [30:26] of the local address are flags and the norm command, not part of the row address
                auto sour_row = Extract(mvout_sour_addr, 25, 0) + (ZExt(start_row, 26)) + (ZExt(start_chunk, 26) * BvConst(DIM, 26));
                auto sour_base = Concat(Extract(mvout_sour_addr, 31, 31), Concat(BvConst(0, 5), sour_row));
                auto full_width = (mvout_source == BvConst(1, 1)) & (Extract(mvout_sour_addr, 29, 29) == BvConst(1, 1)); // bit 29: gemmini/README.md:372-373; gemmini/src/main/scala/gemmini/Scratchpad.scala:312-313

                // Get the row to be transferred
                auto current_row_input = scratchpad.Load(sour_base);
                auto current_row_acc = accumulator.Load(sour_base);

                ExprRef dram_next = DRAM;

                for (size_t elem = 0; elem < DIM; elem++) {
                    auto col = ZExt(col_start, DRAM_ADDR_WIDTH) + BvConst(elem, DRAM_ADDR_WIDTH);
                    auto dram_elem_addr = dram_base + col;

                    auto elem_input = Extract(current_row_input, (elem + 1) * INPUT_BITS - 1, elem * INPUT_BITS);
                    auto pre_scaled_acc = Extract(current_row_acc, (elem + 1) * ACC_BITS - 1, elem * ACC_BITS);
                    // Accumulator read = activation, then acc_scale, then clip to inputType: gemmini/src/main/scala/gemmini/AccumulatorScale.scala:64-83
                    // NOTE: the RTL applies ReLU BEFORE the scale; this code applies it after. They differ for negative scales.
                    auto scaled_acc = FloatScale(pre_scaled_acc, acc_scale, INPUT_BITS);
                    auto elem_acc = Ite(acc_activation == BvConst(1, 2), Relu(scaled_acc), scaled_acc);

                    // Block c is written at byte offset c * DIM: gemmini/src/main/scala/gemmini/DMA.scala:606
                    auto update = Ult(col, ZExt(mvout_col_num, DRAM_ADDR_WIDTH));

                    auto select_source = Ite(mvout_source == BvConst(0, 1),
                        ResizeBv(elem_input, DRAM_DATA_WIDTH),
                        ResizeBv(elem_acc, DRAM_DATA_WIDTH));

                    // Verify: gemmini/README.md:373; gemmini/src/main/scala/gemmini/Scratchpad.scala:312-326 (len * accType bytes, fullAccWriteData)
                    // A full-width accumulator read is not scaled or activated, and is 4 little-endian bytes
                    ExprRef dram_full = dram_next;
                    for (size_t byte = 0; byte < ACC_BITS / 8; byte++) {
                        auto byte_addr = dram_base + col * BvConst(ACC_BITS / 8, DRAM_ADDR_WIDTH) + BvConst(byte, DRAM_ADDR_WIDTH);
                        dram_full = Store(dram_full, byte_addr, ZExt(Extract(pre_scaled_acc, 8 * byte + 7, 8 * byte), DRAM_DATA_WIDTH));
                    }

                    dram_next = Ite(update & continue_row,
                        Ite(full_width, dram_full, Store(dram_next, dram_elem_addr, select_source)),
                        dram_next);
                }

                instr.SetUpdate(DRAM, dram_next);

                // Go to next chunk, then next row
                auto next_chunk = start_chunk + BvConst(1, 16);
                auto chunk_overflow = Uge(next_chunk, total_chunks);
                instr.SetUpdate(start_chunk, Ite(chunk_overflow, BvConst(0, 16), next_chunk));
                instr.SetUpdate(start_row, Ite(chunk_overflow, start_row + BvConst(1, 16), start_row));

                // If rows are all processed, stop
                instr.SetUpdate(done, Ite(!continue_row, BoolConst(true), done));
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
            // set_only_strides = rs1[7]: gemmini/src/main/scala/gemmini/GemminiISA.scala:203-215; only strides are written: ExecuteController.scala:548-565
            auto set_only_strides = Extract(rs1, 7, 7) == BvConst(1, 1);
            auto SetUnlessOnlyStrides = [&](const ExprRef& state, const ExprRef& value) {
                instr.SetUpdate(state, Ite(set_only_strides, state, value));
            };
            SetUnlessOnlyStrides(dataflow, Ite(Extract(rs1, 2, 2) == BvConst(1, 1), BoolConst(true), BoolConst(false)));
            SetUnlessOnlyStrides(activation_func, Extract(rs1, 4, 3)); // rs1[4:3]: gemmini/src/main/scala/gemmini/GemminiISA.scala:194, 212 (README.md:433 says rs1[3])
            SetUnlessOnlyStrides(A_T, Extract(rs1, 8, 8));
            SetUnlessOnlyStrides(B_T, Extract(rs1, 9, 9));
            instr.SetUpdate(A_stride, Extract(rs1, 31, 16));
            SetUnlessOnlyStrides(scalar, Extract(rs1, 63, 32));
            SetUnlessOnlyStrides(right_shift, Extract(rs2, 31, 0));
            instr.SetUpdate(c_stride, Extract(rs2, 63, 48)); // gemmini/src/main/scala/gemmini/GemminiISA.scala:221-225, ExecuteController.scala:565
        }

        {
            // config_mvin
            InstrRef instr = m.NewInstr("config_mvin");
            auto decode = config;
            auto type = Extract(rs1, 1, 0);
            instr.SetDecode((funct == decode) & (type == BvConst(1, 2)));
            // rs1[4:3] = state_id selects the load state: gemmini/README.md:467; gemmini/src/main/scala/gemmini/GemminiISA.scala:111-122,
            //   LoadController.scala:63-64, 143-148
            auto pipeline = Extract(rs1, 4, 3);
            for (size_t p = 0; p < LOAD_PIPELINES; p++) {
                auto selected = pipeline == BvConst(p, 2);
                instr.SetUpdate(acc_type[p], Ite(selected, Extract(rs1, 2, 2), acc_type[p]));
                instr.SetUpdate(private_stride[p], Ite(selected, Extract(rs1, 31, 16), private_stride[p]));
                instr.SetUpdate(memory_stride_mvin[p], Ite(selected, Extract(rs2, 63, 0), memory_stride_mvin[p]));
                instr.SetUpdate(scale[p], Ite(selected, Extract(rs1, 63, 32), scale[p]));
            }
        }

        {
            // config_mvout
            InstrRef instr = m.NewInstr("config_mvout");
            auto decode = config;
            auto type = Extract(rs1, 1, 0);
            instr.SetDecode((funct == decode) & (type == BvConst(2, 2)));
            instr.SetUpdate(acc_activation, Extract(rs1, 3, 2)); // gemmini/src/main/scala/gemmini/GemminiISA.scala:137-150 (activation above cmd_type), StoreController.scala:236
            instr.SetUpdate(max_pool_stride, Extract(rs1, 5, 4));
            instr.SetUpdate(max_pool_window_size, Extract(rs1, 7, 6));
            instr.SetUpdate(upper_zero_pad, Extract(rs1, 9, 8));
            instr.SetUpdate(left_zero_pad, Extract(rs1, 11, 10));
            instr.SetUpdate(out_dim, Extract(rs1, 31, 24));
            instr.SetUpdate(pool_row, Extract(rs1, 39, 32));
            instr.SetUpdate(pool_col, Extract(rs1, 47, 40));
            instr.SetUpdate(unpool_row, Extract(rs1, 55, 48));
            instr.SetUpdate(unpool_col, Extract(rs1, 63, 56));
            instr.SetUpdate(memory_stride_mvout, ZExt(Extract(rs2, 31, 0), 64)); // stride = rs2[31:0]: gemmini/src/main/scala/gemmini/GemminiISA.scala:155-160, StoreController.scala:98-100
            instr.SetUpdate(acc_scale, Extract(rs2, 63, 32)); // gemmini/src/main/scala/gemmini/StoreController.scala:102. NOTE: RTL keeps the old scale if rs2[63:32] is all ones (StoreController.scala:237-239)
        }
    }

    {
        // Core matmul sequence

        // All-high-bits B/D address = zero matrix: gemmini/README.md:532; gemmini/src/main/scala/gemmini/ExecuteController.scala:151-152, 833-834
        // Element (row, col) of the scratchpad; the all-high-bits address reads zeros
        auto SpadElem = [&](const ExprRef& addr, const ExprRef& row, const ExprRef& col) {
            auto row_data = scratchpad.Load(addr + row);
            auto elem = Extract(Lshr(row_data, ResizeBv(col * BvConst(INPUT_BITS, 32), INPUT_ROW_BITS)), INPUT_BITS - 1, 0);
            return Ite(addr == ALL_HIGH_BITS, BvConst(0, INPUT_BITS), elem);
        };
        // Rows/cols outside the operand size are fed as zeros: gemmini/src/main/scala/gemmini/ExecuteController.scala:310-312, 836-838
        auto InBounds = [&](const MatrixOperand& matrix, const ExprRef& i, const ExprRef& j) {
            return Ult(i, ZExt(matrix.rows, 32)) & Ult(j, ZExt(matrix.cols, 32));
        };
        // Transposed operand sizes: gemmini/src/main/scala/gemmini/ExecuteController.scala:161-166
        // matrix[i][j], read from row j if transposed; zero outside the matrix's rows x cols
        auto Elem = [&](const MatrixOperand& matrix, const ExprRef& i, const ExprRef& j, const ExprRef& transposed) {
            auto elem = SpadElem(matrix.addr, Ite(transposed, j, i), Ite(transposed, i, j));
            return Ite(InBounds(matrix, i, j), elem, BvConst(0, INPUT_BITS));
        };
        // A rows are a_addr_stride apart: gemmini/README.md:436-439; gemmini/src/main/scala/gemmini/ExecuteController.scala:370
        // A[i][k]: rows are A_stride apart
        auto ElemA = [&](const MatrixOperand& A, const ExprRef& i, const ExprRef& k) {
            auto transposed = A_T == BvConst(1, 1);
            auto elem = SpadElem(A.addr, ZExt(A_stride, 32) * Ite(transposed, k, i), Ite(transposed, i, k));
            return Ite(InBounds(A, i, k), elem, BvConst(0, INPUT_BITS));
        };
        // B_T transposes D in WS: gemmini/src/main/scala/gemmini/ExecuteController.scala:129, 165-166. Signed: Configs.scala:23 (SInt(8.W))
        // Value preloaded into PE (i, j): D[i][j] (OS) or B[i][j] (WS, transposed if B_T)
        auto PreloadElem = [&](size_t i, size_t j) {
            MatrixOperand preloaded { preload_addr, preload_row, preload_col };
            auto transposed = dataflow & (B_T == BvConst(1, 1));
            return SExt(Elem(preloaded, BvConst(i, 32), BvConst(j, 32), transposed), ACC_BITS);
        };

        // Output row address uses c_addr_stride: gemmini/src/main/scala/gemmini/ExecuteController.scala:907-908
        // Row i of the C operand of matmul.preload: rows are c_stride apart, bits [30:29] are flags
        auto OutputRow = [&](const ExprRef& i) {
            auto row = Extract(dest_addr, 28, 0) + ZExt(c_stride, 29) * Extract(i, 28, 0);
            return Concat(Extract(dest_addr, 31, 31), Concat(BvConst(0, 2), row));
        };
        // Rounding shift (OS only): gemmini/src/main/scala/gemmini/PE.scala:104,111, Arithmetic.scala:97-108
        // Saturate to inputType, then ReLU: gemmini/src/main/scala/gemmini/ExecuteController.scala:926-928
        // A result written to the scratchpad is rounding right-shifted, saturated and activated
        auto ScratchpadResult = [&](const ExprRef& value, const ExprRef& shift) {
            auto shifted = Saturate(RoundShiftRightEven(value, shift), INPUT_BITS);
            return Ite(activation_func == BvConst(1, 2), Relu(shifted), shifted);
        };
        // Accumulator write: gemmini/README.md:371; gemmini/src/main/scala/gemmini/ExecuteController.scala:951-952
        // A result written to the accumulator is added to it if bit 30 is set
        auto AccumulatorResult = [&](const ExprRef& value, const ExprRef& old_value) {
            return Ite(Extract(dest_addr, 30, 30) == BvConst(1, 1), old_value + value, value);
        };

        {
            // Preload runs together with the next compute: gemmini/README.md:515-516; gemmini/src/main/scala/gemmini/ExecuteController.scala:84-87
            // matmul.preload: records the D/B and C operands; the PEs are loaded by matmul.compute.preloaded
            InstrRef instr = m.NewInstr("matmul.preload");
            auto decode = matmul_preload;
            instr.SetDecode(funct == decode);
            instr.SetUpdate(preload_addr, Extract(rs1, 31, 0));
            instr.SetUpdate(preload_col, Extract(rs1, 47, 32));
            instr.SetUpdate(preload_row, Extract(rs1, 63, 48));
            instr.SetUpdate(dest_addr, Extract(rs2, 31, 0));
            instr.SetUpdate(dest_col, Extract(rs2, 47, 32));
            instr.SetUpdate(dest_row, Extract(rs2, 63, 48));
        }

        // ILA modeling choice: one model per ILA. The old atomic decode (funct 1) is LOAD2_CMD: gemmini/src/main/scala/gemmini/GemminiISA.scala:9
        if (compute_model == ComputeModel::Atomic) {
            // matmul.compute.preloaded in one step
            InstrRef instr = m.NewInstr("matmul.compute.atomic");
            auto decode = matmul_compute_preloaded;
            instr.SetDecode(funct == decode);
            auto A_scratchpad_addr = Extract(rs1, 31, 0);
            auto A_col_ = Extract(rs1, 47, 32);
            auto A_row_ = Extract(rs1, 63, 48);
            auto BD_scratchpad_addr = Extract(rs2, 31, 0);
            auto BD_col = Extract(rs2, 47, 32);
            auto BD_row = Extract(rs2, 63, 48);
            MatrixOperand A { A_scratchpad_addr, A_row_, A_col_ };
            MatrixOperand BD { BD_scratchpad_addr, BD_row, BD_col };

            auto os_mode = (dataflow == BoolConst(false));
            auto B_transpose = (B_T == BvConst(1, 1));
            auto shift = Ite(os_mode, right_shift, BvConst(0, 32)); // WS output is not shifted: gemmini/src/main/scala/gemmini/PE.scala:120,126

            auto new_scratchpad = scratchpad;
            auto new_accumulator = accumulator;

            std::vector<std::vector<ExprRef>> preloaded(DIM);
            for (size_t i = 0; i < DIM; i++)
                for (size_t j = 0; j < DIM; j++)
                    preloaded[i].push_back(PreloadElem(i, j));

            for (size_t i = 0; i < DIM; i++) {
                auto& sys_row = sys_array[i];

                std::vector<ExprRef> sp_row_elems(DIM, BvConst(0, INPUT_BITS));
                std::vector<ExprRef> acc_row_elems(DIM, BvConst(0, ACC_BITS));
                std::vector<ExprRef> row_write_valid(DIM, SYMB_FALSE);

                for (size_t j = 0; j < DIM; j++) {
                    // OS: c1 starts at D and accumulates A*B: gemmini/src/main/scala/gemmini/PE.scala:102-117. WS: B is held, D flows down: PE.scala:118-131
                    // OS mode: A*B accumulated onto the D preloaded into the PE
                    // WS mode: A times the B preloaded into the PEs, plus D
                    ExprRef os_sum = preloaded[i][j];
                    ExprRef ws_sum = SExt(Elem(BD, BvConst(i, 32), BvConst(j, 32), SYMB_FALSE), ACC_BITS);
                    for (size_t k = 0; k < DIM; k++) {
                        auto a_elem = SExt(ElemA(A, BvConst(i, 32), BvConst(k, 32)), ACC_BITS);
                        auto b_elem = SExt(Elem(BD, BvConst(k, 32), BvConst(j, 32), B_transpose), ACC_BITS);
                        os_sum = os_sum + a_elem * b_elem;
                        ws_sum = ws_sum + a_elem * preloaded[k][j];
                    }
                    auto combined_val = Ite(os_mode, os_sum, ws_sum);

                    // OS mode: result stays in the PE (stationary). WS mode: the PE keeps the weight.
                    instr.SetUpdate(sys_row[j]->stationary_reg, Ite(os_mode, os_sum, preloaded[i][j]));

                    row_write_valid[j] = (BvConst(i, 16) < dest_row) & (BvConst(j, 16) < dest_col); // C rows/cols mask: gemmini/src/main/scala/gemmini/ExecuteController.scala:919-921
                    sp_row_elems[j] = ScratchpadResult(combined_val, shift);
                    acc_row_elems[j] = combined_val;
                }

                auto to_scratchpad = (Extract(dest_addr, 31, 31) == BvConst(0, 1));

                auto old_sp_row = new_scratchpad.Load(OutputRow(BvConst(i, 32)));
                auto old_acc_row = new_accumulator.Load(OutputRow(BvConst(i, 32)));

                ExprRef sp_row_val = BvConst(0, INPUT_ROW_BITS);
                ExprRef acc_row_val = BvConst(0, ACC_ROW_BITS);
                for (size_t j = 0; j < DIM; j++) {
                    auto old_sp_elem = Extract(old_sp_row, (j + 1) * INPUT_BITS - 1, j * INPUT_BITS);
                    auto old_acc_elem = Extract(old_acc_row, (j + 1) * ACC_BITS - 1, j * ACC_BITS);

                    auto sp_elem = Ite(row_write_valid[j], sp_row_elems[j], old_sp_elem);
                    auto acc_elem = Ite(row_write_valid[j], AccumulatorResult(acc_row_elems[j], old_acc_elem), old_acc_elem);

                    sp_row_val = (j == 0) ? sp_elem : Concat(sp_elem, sp_row_val); // ASSUMPTION: Concat(hi, lo)
                    acc_row_val = (j == 0) ? acc_elem : Concat(acc_elem, acc_row_val);
                }

                new_scratchpad = Ite(to_scratchpad, new_scratchpad.Store(OutputRow(BvConst(i, 32)), sp_row_val), new_scratchpad);
                new_accumulator = Ite(!to_scratchpad, new_accumulator.Store(OutputRow(BvConst(i, 32)), acc_row_val), new_accumulator);
            }

            instr.SetUpdate(scratchpad, Ite(dest_addr != ALL_HIGH_BITS, new_scratchpad, scratchpad));
            instr.SetUpdate(accumulator, Ite(dest_addr != ALL_HIGH_BITS, new_accumulator, accumulator));
        }

        if (compute_model == ComputeModel::Stepped) {

            {
                // matmul.compute.preloaded
                InstrRef instr = m.NewInstr("matmul.compute.preloaded");
                auto decode = matmul_compute_preloaded;
                instr.SetDecode(funct == decode & (busy == BoolConst(false)));
                auto A_scratchpad_addr = Extract(rs1, 31, 0);
                auto A_col_ = Extract(rs1, 47, 32);
                auto A_row_ = Extract(rs1, 63, 48);
                auto BD_scratchpad_addr = Extract(rs2, 31, 0);
                auto BD_col = Extract(rs2, 47, 32);
                auto BD_row = Extract(rs2, 63, 48);

                instr.SetUpdate(A_addr, A_scratchpad_addr);
                instr.SetUpdate(A_row, A_row_);
                instr.SetUpdate(A_col, A_col_);
                instr.SetUpdate(B_D_addr, BD_scratchpad_addr);
                instr.SetUpdate(B_D_row, BD_row);
                instr.SetUpdate(B_D_col, BD_col);
                instr.SetUpdate(cycle, BvConst(0, 32));
                instr.SetUpdate(busy, BoolConst(true));
                // The preloaded D/B enters the PEs when the compute starts: gemmini/src/main/scala/gemmini/ExecuteController.scala:84-87
                for (size_t i = 0; i < DIM; i++)
                    for (size_t j = 0; j < DIM; j++)
                        instr.SetUpdate(sys_array[i][j]->stationary_reg, PreloadElem(i, j));
            }

            {

                // matmul.compute.preloaded step
                InstrRef instr = m.NewInstr("matmul.compute.preloaded_step");
                auto decode = funct == matmul_compute_preloaded & (busy == BoolConst(true))
                    // ILA timing, not architectural: last input reaches PE (DIM-1, DIM-1) at cycle 3*DIM-3
                    & (cycle <= BvConst((3 * DIM) - 2, 32));
                instr.SetDecode(decode);
                auto os_mode = (dataflow == BoolConst(false));
                auto write_cycle = os_mode & (cycle == BvConst((3 * DIM) - 2, 32));
                auto destination = Extract(dest_addr, 31, 31);

                ExprRef scratchpad_next2 = scratchpad;
                ExprRef accumulator_next2 = accumulator;

                auto B_transpose = B_T == BvConst(1, 1);
                MatrixOperand A { A_addr, A_row, A_col };
                MatrixOperand BD { B_D_addr, B_D_row, B_D_col };

                for (size_t row = 0; row < DIM; row++) {
                    auto is_last_row = row == DIM - 1 ? SYMB_TRUE : SYMB_FALSE;

                    for (size_t col = 0; col < DIM; col++) {
                        ExprRef A_in = BvConst(0, INPUT_BITS);
                        ExprRef B_D_in = BvConst(0, INPUT_BITS);

                        if (col == 0) {
                            // OS: A[row][cycle - row]. WS: A[cycle - row][row]
                            auto skewed = cycle - BvConst(row, 32);
                            auto A_in_os = ElemA(A, BvConst(row, 32), skewed);
                            auto A_in_ws = ElemA(A, skewed, BvConst(row, 32));
                            A_in = Ite(os_mode, A_in_os, A_in_ws);
                        } else {
                            A_in = sys_array[row][col - 1]->A_reg;
                        }

                        if (row == 0) {
                            // B[cycle - col][col]
                            B_D_in = Elem(BD, cycle - BvConst(col, 32), BvConst(col, 32), B_transpose);
                        } else {
                            B_D_in = sys_array[row - 1][col]->B_D_reg;
                        }

                        instr.SetUpdate(sys_array[row][col]->A_reg, Ite(!write_cycle, A_in, sys_array[row][col]->A_reg));
                        instr.SetUpdate(sys_array[row][col]->B_D_reg, Ite(!write_cycle, B_D_in, sys_array[row][col]->B_D_reg));

                        // Operands are signed (inputType = SInt(8.W)): gemmini/src/main/scala/gemmini/Configs.scala:23-25, PE.scala:23 (mac)
                        auto weight = Ite(os_mode, SExt(B_D_in, ACC_BITS), sys_array[row][col]->stationary_reg);
                        auto product = SExt(A_in, ACC_BITS) * weight;

                        auto stat_updated = sys_array[row][col]->stationary_reg + product;
                        instr.SetUpdate(sys_array[row][col]->stationary_reg,
                            Ite(os_mode & !write_cycle, stat_updated, sys_array[row][col]->stationary_reg));
                        auto c_os = stat_updated;

                        ExprRef psum_in = BvConst(0, ACC_BITS);
                        if (row == 0) {
                            // D[cycle - col][col]
                            psum_in = SExt(Elem(BD, cycle - BvConst(col, 32), BvConst(col, 32), SYMB_FALSE), ACC_BITS);
                        } else {
                            // Psum flows down without a ZExt: gemmini/src/main/scala/gemmini/PE.scala:123,129
                            psum_in = sys_array[row - 1][col]->C_reg_out;
                        }
                        // Psums are not truncated to 16 bits: gemmini/src/main/scala/gemmini/PE.scala:65 (WS: outputType = SInt(20.W), Configs.scala:29)
                        auto c_ws = product + psum_in;

                        instr.SetUpdate(sys_array[row][col]->C_reg_out,
                            Ite(!write_cycle, Ite(os_mode, c_os, c_ws), sys_array[row][col]->C_reg_out));

                        auto row_off = BvConst(row, 32);
                        auto col_off = BvConst(col, 32);
                        auto instance = cycle - row_off - col_off;
                        auto col_valid = is_last_row
                            & (cycle >= row_off + col_off)
                            & (instance < ZExt(dest_row, 32))
                            & Ite((BvConst(col, 16) < dest_col), SYMB_TRUE, SYMB_FALSE);
                        auto writeAddrCol = OutputRow(instance); // gemmini/src/main/scala/gemmini/ExecuteController.scala:907-908 (c_addr_stride)

                        auto existingRowSp = scratchpad_next2.Load(writeAddrCol);
                        auto existingRowAcc = accumulator_next2.Load(writeAddrCol);
                        ExprRef newRowSp = BvConst(0, INPUT_ROW_BITS);
                        ExprRef newRowAcc = BvConst(0, ACC_ROW_BITS);
                        for (size_t c = 0; c < DIM; c++) {
                            auto existingElemSp = Extract(existingRowSp, (c + 1) * INPUT_BITS - 1, c * INPUT_BITS);
                            auto existingElemAcc = Extract(existingRowAcc, (c + 1) * ACC_BITS - 1, c * ACC_BITS);
                            auto elemSp = (c == col) ? ScratchpadResult(c_ws, BvConst(0, ACC_BITS)) : existingElemSp;
                            auto elemAcc = (c == col) ? AccumulatorResult(c_ws, existingElemAcc) : existingElemAcc;
                            if (c == 0) {
                                newRowSp = elemSp;
                                newRowAcc = elemAcc;
                            } else {
                                newRowSp = Concat(elemSp, newRowSp);
                                newRowAcc = Concat(elemAcc, newRowAcc);
                            }
                        }

                        scratchpad_next2 = Ite(!os_mode & (destination == BvConst(0, 1)) & col_valid,
                            scratchpad_next2.Store(writeAddrCol, newRowSp), scratchpad_next2);
                        accumulator_next2 = Ite(!os_mode & (destination == BvConst(1, 1)) & col_valid,
                            accumulator_next2.Store(writeAddrCol, newRowAcc), accumulator_next2);
                    }
                }

                // OS MODE WRITE BACK
                ExprRef scratchpad_next = scratchpad;
                ExprRef accumulator_next = accumulator;
                for (size_t row = 0; row < DIM; row++) {
                    auto writeAddr = OutputRow(BvConst(row, 32));
                    auto destRowSp = scratchpad.Load(writeAddr);
                    auto destRowAcc = accumulator.Load(writeAddr);
                    ExprRef newRowSp = BvConst(0, INPUT_ROW_BITS);
                    ExprRef newRowAcc = BvConst(0, ACC_ROW_BITS);
                    for (size_t col = 0; col < DIM; col++) {
                        auto should_transfer = Ite((BvConst(row, 16) < dest_row) & (BvConst(col, 16) < dest_col), SYMB_TRUE, SYMB_FALSE);
                        auto existingElemSp = Extract(destRowSp, (col + 1) * INPUT_BITS - 1, col * INPUT_BITS);
                        auto existingElemAcc = Extract(destRowAcc, (col + 1) * ACC_BITS - 1, col * ACC_BITS);
                        auto C_elem = sys_array[row][col]->C_reg_out;
                        // OS output: gemmini/src/main/scala/gemmini/PE.scala:104,111 (rounding shift), ExecuteController.scala:926-928 (clip, ReLU)
                        auto final_output = ScratchpadResult(C_elem, right_shift);
                        auto final_output_acc = AccumulatorResult(C_elem, existingElemAcc);
                        if (col == 0) {
                            newRowSp = Ite(should_transfer, final_output, existingElemSp);
                            newRowAcc = Ite(should_transfer, final_output_acc, existingElemAcc);
                        } else {
                            newRowSp = Ite(should_transfer, Concat(final_output, newRowSp), Concat(existingElemSp, newRowSp));
                            newRowAcc = Ite(should_transfer, Concat(final_output_acc, newRowAcc), Concat(existingElemAcc, newRowAcc));
                        }
                    }
                    scratchpad_next = Ite(os_mode & (destination == BvConst(0, 1)) & write_cycle,
                        scratchpad_next.Store(writeAddr, newRowSp), scratchpad_next);
                    accumulator_next = Ite(os_mode & (destination == BvConst(1, 1)) & write_cycle,
                        accumulator_next.Store(writeAddr, newRowAcc), accumulator_next);
                }
                instr.SetUpdate(scratchpad, Ite(dest_addr != ALL_HIGH_BITS, Ite(os_mode, scratchpad_next, scratchpad_next2), scratchpad));
                instr.SetUpdate(accumulator, Ite(dest_addr != ALL_HIGH_BITS, Ite(os_mode, accumulator_next, accumulator_next2), accumulator));

                instr.SetUpdate(busy, Ite(cycle == 3 * DIM - 2, BoolConst(false), busy));
                instr.SetUpdate(cycle, cycle + BvConst(1, 32));
            }
        }

        {
            {
                // matmul.compute.accumulated
                InstrRef instr = m.NewInstr("matmul.compute.accumulated");
                auto decode = matmul_compute_accumulated;
                instr.SetDecode(funct == decode & (busy == BoolConst(false)));
                auto A_scratchpad_addr = Extract(rs1, 31, 0);
                auto A_col_ = Extract(rs1, 47, 32);
                auto A_row_ = Extract(rs1, 63, 48);
                auto BD_scratchpad_addr = Extract(rs2, 31, 0);
                auto BD_col = Extract(rs2, 47, 32);
                auto BD_row = Extract(rs2, 63, 48);

                instr.SetUpdate(A_addr, A_scratchpad_addr);
                instr.SetUpdate(A_row, A_row_);
                instr.SetUpdate(A_col, A_col_);
                instr.SetUpdate(B_D_addr, BD_scratchpad_addr);
                instr.SetUpdate(B_D_row, BD_row);
                instr.SetUpdate(B_D_col, BD_col);
                instr.SetUpdate(cycle, BvConst(0, 32));
                instr.SetUpdate(busy, BoolConst(true));
            }

            {
                // matmul.compute.accumulated step
                InstrRef instr = m.NewInstr("matmul.compute.accumulated_step");
                auto decode = funct == matmul_compute_accumulated & (busy == BoolConst(true))
                    // ILA timing, not architectural: last input reaches PE (DIM-1, DIM-1) at cycle 3*DIM-3
                    & (cycle <= BvConst((3 * DIM) - 2, 32));
                instr.SetDecode(decode);
                auto os_mode = (dataflow == BoolConst(false));
                auto write_cycle = os_mode & (cycle == BvConst((3 * DIM) - 2, 32));
                auto destination = Extract(dest_addr, 31, 31);

                ExprRef scratchpad_next2 = scratchpad;
                ExprRef accumulator_next2 = accumulator;

                auto B_transpose = B_T == BvConst(1, 1);
                MatrixOperand A { A_addr, A_row, A_col };
                MatrixOperand BD { B_D_addr, B_D_row, B_D_col };

                for (size_t row = 0; row < DIM; row++) {
                    auto is_last_row = row == DIM - 1 ? SYMB_TRUE : SYMB_FALSE;

                    for (size_t col = 0; col < DIM; col++) {
                        ExprRef A_in = BvConst(0, INPUT_BITS);
                        ExprRef B_D_in = BvConst(0, INPUT_BITS);

                        if (col == 0) {
                            // OS: A[row][cycle - row]. WS: A[cycle - row][row]
                            auto skewed = cycle - BvConst(row, 32);
                            auto A_in_os = ElemA(A, BvConst(row, 32), skewed);
                            auto A_in_ws = ElemA(A, skewed, BvConst(row, 32));
                            A_in = Ite(os_mode, A_in_os, A_in_ws);
                        } else {
                            A_in = sys_array[row][col - 1]->A_reg;
                        }

                        if (row == 0) {
                            // B[cycle - col][col]
                            B_D_in = Elem(BD, cycle - BvConst(col, 32), BvConst(col, 32), B_transpose);
                        } else {
                            B_D_in = sys_array[row - 1][col]->B_D_reg;
                        }

                        instr.SetUpdate(sys_array[row][col]->A_reg, Ite(!write_cycle, A_in, sys_array[row][col]->A_reg));
                        instr.SetUpdate(sys_array[row][col]->B_D_reg, Ite(!write_cycle, B_D_in, sys_array[row][col]->B_D_reg));

                        // Operands are signed (inputType = SInt(8.W)): gemmini/src/main/scala/gemmini/Configs.scala:23-25, PE.scala:23 (mac)
                        auto weight = Ite(os_mode, SExt(B_D_in, ACC_BITS), sys_array[row][col]->stationary_reg);
                        auto product = SExt(A_in, ACC_BITS) * weight;

                        auto stat_updated = sys_array[row][col]->stationary_reg + product;
                        instr.SetUpdate(sys_array[row][col]->stationary_reg,
                            Ite(os_mode & !write_cycle, stat_updated, sys_array[row][col]->stationary_reg));
                        auto c_os = stat_updated;

                        ExprRef psum_in = BvConst(0, ACC_BITS);
                        if (row == 0) {
                            // D[cycle - col][col]
                            psum_in = SExt(Elem(BD, cycle - BvConst(col, 32), BvConst(col, 32), SYMB_FALSE), ACC_BITS);
                        } else {
                            // Psum flows down without a ZExt: gemmini/src/main/scala/gemmini/PE.scala:123,129
                            psum_in = sys_array[row - 1][col]->C_reg_out;
                        }
                        // Psums are not truncated to 16 bits: gemmini/src/main/scala/gemmini/PE.scala:65 (WS: outputType = SInt(20.W), Configs.scala:29)
                        auto c_ws = product + psum_in;

                        instr.SetUpdate(sys_array[row][col]->C_reg_out,
                            Ite(!write_cycle, Ite(os_mode, c_os, c_ws), sys_array[row][col]->C_reg_out));

                        auto row_off = BvConst(row, 32);
                        auto col_off = BvConst(col, 32);
                        auto instance = cycle - row_off - col_off;
                        auto col_valid = is_last_row
                            & (cycle >= row_off + col_off)
                            & (instance < ZExt(dest_row, 32)) 
                            & Ite((BvConst(col, 16) < dest_col), SYMB_TRUE, SYMB_FALSE);
                        auto writeAddrCol = OutputRow(instance); // gemmini/src/main/scala/gemmini/ExecuteController.scala:907-908 (c_addr_stride)

                        auto existingRowSp = scratchpad_next2.Load(writeAddrCol);
                        auto existingRowAcc = accumulator_next2.Load(writeAddrCol);
                        ExprRef newRowSp = BvConst(0, INPUT_ROW_BITS);
                        ExprRef newRowAcc = BvConst(0, ACC_ROW_BITS);
                        for (size_t c = 0; c < DIM; c++) {
                            auto existingElemSp = Extract(existingRowSp, (c + 1) * INPUT_BITS - 1, c * INPUT_BITS);
                            auto existingElemAcc = Extract(existingRowAcc, (c + 1) * ACC_BITS - 1, c * ACC_BITS);
                            auto elemSp = (c == col) ? ScratchpadResult(c_ws, BvConst(0, ACC_BITS)) : existingElemSp;
                            auto elemAcc = (c == col) ? AccumulatorResult(c_ws, existingElemAcc) : existingElemAcc;
                            if (c == 0) {
                                newRowSp = elemSp;
                                newRowAcc = elemAcc;
                            } else {
                                newRowSp = Concat(elemSp, newRowSp);
                                newRowAcc = Concat(elemAcc, newRowAcc);
                            }
                        }

                        scratchpad_next2 = Ite(!os_mode & (destination == BvConst(0, 1)) & col_valid,
                            scratchpad_next2.Store(writeAddrCol, newRowSp), scratchpad_next2);
                        accumulator_next2 = Ite(!os_mode & (destination == BvConst(1, 1)) & col_valid,
                            accumulator_next2.Store(writeAddrCol, newRowAcc), accumulator_next2);
                    }
                }

                // OS MODE WRITE BACK
                ExprRef scratchpad_next = scratchpad;
                ExprRef accumulator_next = accumulator;
                for (size_t row = 0; row < DIM; row++) {
                    auto writeAddr = OutputRow(BvConst(row, 32));
                    auto destRowSp = scratchpad.Load(writeAddr);
                    auto destRowAcc = accumulator.Load(writeAddr);
                    ExprRef newRowSp = BvConst(0, INPUT_ROW_BITS);
                    ExprRef newRowAcc = BvConst(0, ACC_ROW_BITS);
                    for (size_t col = 0; col < DIM; col++) {
                        auto should_transfer = Ite((BvConst(row, 16) < dest_row) & (BvConst(col, 16) < dest_col), SYMB_TRUE, SYMB_FALSE);
                        auto existingElemSp = Extract(destRowSp, (col + 1) * INPUT_BITS - 1, col * INPUT_BITS);
                        auto existingElemAcc = Extract(destRowAcc, (col + 1) * ACC_BITS - 1, col * ACC_BITS);
                        auto C_elem = sys_array[row][col]->C_reg_out;
                        // OS output: gemmini/src/main/scala/gemmini/PE.scala:104,111 (rounding shift), ExecuteController.scala:926-928 (clip, ReLU)
                        auto final_output = ScratchpadResult(C_elem, right_shift);
                        auto final_output_acc = AccumulatorResult(C_elem, existingElemAcc);
                        if (col == 0) {
                            newRowSp = Ite(should_transfer, final_output, existingElemSp);
                            newRowAcc = Ite(should_transfer, final_output_acc, existingElemAcc);
                        } else {
                            newRowSp = Ite(should_transfer, Concat(final_output, newRowSp), Concat(existingElemSp, newRowSp));
                            newRowAcc = Ite(should_transfer, Concat(final_output_acc, newRowAcc), Concat(existingElemAcc, newRowAcc));
                        }
                    }
                    scratchpad_next = Ite(os_mode & (destination == BvConst(0, 1)) & write_cycle,
                        scratchpad_next.Store(writeAddr, newRowSp), scratchpad_next);
                    accumulator_next = Ite(os_mode & (destination == BvConst(1, 1)) & write_cycle,
                        accumulator_next.Store(writeAddr, newRowAcc), accumulator_next);
                }
                instr.SetUpdate(scratchpad, Ite(dest_addr != ALL_HIGH_BITS, Ite(os_mode, scratchpad_next, scratchpad_next2), scratchpad));
                instr.SetUpdate(accumulator, Ite(dest_addr != ALL_HIGH_BITS, Ite(os_mode, accumulator_next, accumulator_next2), accumulator));

                instr.SetUpdate(busy, Ite(cycle == 3 * DIM - 2, BoolConst(false), busy));
                instr.SetUpdate(cycle, cycle + BvConst(1, 32));
            }
        }
    }
}

}
