/*******************************************************************************
* Copyright 2019-2021 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_X64_RNN_JIT_UNI_LSTM_CELL_POSTGEMM_BWD_HPP
#define CPU_X64_RNN_JIT_UNI_LSTM_CELL_POSTGEMM_BWD_HPP

#include <memory>
#include "common/utils.hpp"
#include "cpu/x64/rnn/jit_uni_rnn_common_postgemm.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

template <cpu_isa_t isa, impl::data_type_t src_data_t,
        impl::data_type_t scratch_data_t>
struct jit_uni_lstm_cell_postgemm_bwd : public jit_uni_rnn_postgemm {
    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_uni_lstm_cell_postgemm_bwd)

    jit_uni_lstm_cell_postgemm_bwd(
            const rnn_utils::rnn_conf_t &rnn, const rnn_pd_t *pd)
        : jit_uni_rnn_postgemm(rnn, pd), tanh_injector_(nullptr) {}
    ~jit_uni_lstm_cell_postgemm_bwd() = default;

    status_t init(data_type_t sdt) override {
        jit_uni_rnn_postgemm::init(src_data_t);
        // we use rax for both constant tables as they use the same table
        tanh_injector_ = utils::make_unique<injector_t>(
                this, alg_kind::eltwise_tanh, 0.0f, 0.0f, 1.0f, true, rax);
        return create_kernel();
    }

protected:
    using injector_t = typename utils::conditional<isa == avx512_core,
            jit_uni_eltwise_injector_f32<avx512_common>,
            jit_uni_eltwise_injector_f32<isa>>::type;
    using Vmm = typename cpu_isa_traits<isa>::Vmm;

    std::unique_ptr<injector_t> tanh_injector_;

    // register size in bytes
    static constexpr size_t vlen_ = cpu_isa_traits<isa>::vlen;
    static constexpr size_t cstate_dt_size_ = sizeof(float);
    static constexpr size_t hstate_dt_size_ = sizeof(float);
    static constexpr size_t weights_peephole_dt_size_ = sizeof(float);
    static constexpr int tmp_id_begin_ = 11;
    const int tmp_id_end_ = cpu_isa_traits<isa>::n_vregs
            - ((bf16_emu_ && is_superset(isa, avx512_common)) ? 4 : 0);

    const size_t vlen_scratch_
            = vlen_ / (sizeof(float) / types::data_type_size(scratch_data_t));
    const size_t gate_dt_size_ = types::data_type_size(scratch_data_t);
    const size_t scratch_dt_size_ = types::data_type_size(scratch_data_t);

    int current_tmp_id_ = tmp_id_begin_;
    const bool avx2_available_ = is_superset(isa, avx2);

    Vmm get_next_tmp_vmm() {
        const Vmm vmm {current_tmp_id_++};

        if (current_tmp_id_ == tmp_id_end_) current_tmp_id_ = tmp_id_begin_;

        return vmm;
    }

    Xbyak::Xmm get_next_tmp_xmm() {
        return Xbyak::Xmm(get_next_tmp_vmm().getIdx());
    }

    void vaddps_rhs_op_mem(
            const Vmm &dst, const Vmm &lhs, const Xbyak::Address &rhs_addr) {

        if (avx2_available_)
            uni_vaddps(dst, lhs, rhs_addr);
        else {
            const auto rhs = get_next_tmp_vmm();
            uni_vmovups(rhs, rhs_addr);
            uni_vaddps(dst, lhs, rhs);
        }
    }

    void vfmadd231ps_rhs_op_mem(
            const Vmm &dst, const Vmm &lhs, const Xbyak::Address &rhs_addr) {
        if (avx2_available_)
            uni_vfmadd231ps(dst, lhs, rhs_addr);
        else {
            const auto rhs = get_next_tmp_vmm();
            uni_vmovups(rhs, rhs_addr);
            uni_vfmadd231ps(dst, lhs, rhs);
        }
    }

    void vmulps_rhs_op_mem(
            const Vmm &dst, const Vmm &lhs, const Xbyak::Address &rhs_addr) {
        if (avx2_available_)
            uni_vmulps(dst, lhs, rhs_addr);
        else {
            const auto rhs = get_next_tmp_vmm();
            uni_vmovups(rhs, rhs_addr);
            uni_vmulps(dst, lhs, rhs);
        }
    }

    void vaddss_rhs_op_mem(const Xbyak::Xmm &dst, const Xbyak::Xmm &lhs,
            const Xbyak::Address &rhs_addr) {
        if (avx2_available_)
            uni_vaddss(dst, lhs, rhs_addr);
        else {
            const auto rhs = get_next_tmp_xmm();
            uni_vmovss(rhs, rhs_addr);
            uni_vaddss(dst, lhs, rhs);
        }
    }

    void vfmadd231ss_rhs_op_mem(const Xbyak::Xmm &dst, const Xbyak::Xmm &lhs,
            const Xbyak::Address &rhs_addr) {
        if (avx2_available_)
            uni_vfmadd231ss(dst, lhs, rhs_addr);
        else {
            const auto rhs = get_next_tmp_xmm();
            uni_vmovss(rhs, rhs_addr);
            uni_vfmadd231ss(dst, lhs, rhs);
        }
    }

    void vmulss_rhs_op_mem(const Xbyak::Xmm &dst, const Xbyak::Xmm &lhs,
            const Xbyak::Address &rhs_addr) {
        if (avx2_available_)
            uni_vmulss(dst, lhs, rhs_addr);
        else {
            const auto rhs = get_next_tmp_xmm();
            uni_vmovss(rhs, rhs_addr);
            uni_vmulss(dst, lhs, rhs);
        }
    }

    void generate() override {
        using namespace Xbyak;

        // Labels declaration
        Label vector_loop_start_label, vector_loop_end_label;
        Label rem_loop_start_label, rem_loop_end_label;
        Label table_label;

        // Register map
        const Reg64 table_reg(rbx); // used to load ones before the loop
        const Reg64 loop_cnt(
                rbx); // loop counter, can be aliased with table_reg
        // We skip vmm0 as it can be used by the injector for masks on sse4.1
        const int dG0_idx = 1, dG1_idx = 2, dG2_idx = 3, dG3_idx = 4,
                  tanhCt_idx = 5, dHt_idx = 6, dCt_idx = 7, G0_idx = 8,
                  G1_idx = 9, one_idx = 10;
        const Vmm one_vmm(one_idx);
        const Xmm one_xmm(one_idx);

        // Adress maping
        const Address one_addr = ptr[table_reg];
        // We start code generations here
        preamble();

        // extract addresses passed as parameter
        const auto addr_ws_gates_reg = abi_param1;
        const auto addr_scratch_gates_reg = abi_param2;
        const auto addr_diff_states_t_lp1_reg = abi_param3;
        const auto addr_diff_states_tp1_l_reg = abi_param4;
        const auto addr_weights_peephole_reg = r12;
#ifdef _WIN32
        const auto addr_diff_c_states_t_l_reg = r10;
        const auto addr_diff_c_states_tp1_l_reg = r11;
        const auto addr_c_states_tm1_l_reg = rdi;
        const auto addr_c_states_t_l_reg = rsi;
        const auto base_args = get_stack_params_address();
        mov(addr_diff_c_states_t_l_reg, ptr[base_args]);
        mov(addr_diff_c_states_tp1_l_reg, ptr[base_args + 8]);
        mov(addr_c_states_tm1_l_reg, ptr[base_args + 16]);
        mov(addr_c_states_t_l_reg, ptr[base_args + 24]);
        mov(addr_weights_peephole_reg, ptr[base_args + 32]);
#else
        const auto addr_diff_c_states_t_l_reg = abi_param5;
        const auto addr_diff_c_states_tp1_l_reg = abi_param6;
        const auto addr_c_states_tm1_l_reg = r10;
        const auto addr_c_states_t_l_reg = r11;
        const auto base_args = get_stack_params_address();
        mov(addr_c_states_tm1_l_reg, ptr[base_args]);
        mov(addr_c_states_t_l_reg, ptr[base_args + 8]);
        mov(addr_weights_peephole_reg, ptr[base_args + 16]);
#endif

        // helper lambda to address the gates and biases
        const auto sg_addr = [&](int i) {
            return ptr[addr_scratch_gates_reg
                    + i * rnn_.dhc * scratch_dt_size_];
        };
        const auto weights_peephole_addr = [&](int i) {
            return ptr[addr_weights_peephole_reg
                    + i * rnn_.dhc * weights_peephole_dt_size_];
        };
        const auto wg_addr = [&](int i) {
            return ptr[addr_ws_gates_reg + i * rnn_.dhc * gate_dt_size_];
        };

        // initialize registers with addresses and constants
        mov(table_reg, table_label);
        init_regs(vlen_);
        uni_vmovups(one_vmm, one_addr);
        tanh_injector_->load_table_addr();

        mov(loop_cnt, rnn_.dhc * scratch_dt_size_);
        cmp(loop_cnt, vlen_scratch_);
        jl(vector_loop_end_label, Xbyak::CodeGenerator::T_NEAR);

        L(vector_loop_start_label);
        {
            const Vmm dG0(dG0_idx), dG1(dG1_idx), dG2(dG2_idx), dG3(dG3_idx),
                    tanhCt(tanhCt_idx), dHt(dHt_idx), dCt(dCt_idx), G0(G0_idx),
                    G1(G1_idx);

            // TODO: if w_gates are bfloat, we have to convert them to float
            // datatypes summary:
            // - c states are all float
            // - h states are all src_data_t
            // - diff_* are all float
            // - scratch is src_data_t
            // - ws_gates is src_data_t

            // compute tanhCt
            uni_vmovups(tanhCt, ptr[addr_c_states_t_l_reg]);
            tanh_injector_->compute_vector(tanhCt.getIdx());

            // compute dHt
            // assumption: the diff_states_t_lp1 address is already offset by rnn.n_states
            uni_vmovups(dHt, ptr[addr_diff_states_t_lp1_reg]);
            if (!rnn_.is_lstm_projection) {
                vaddps_rhs_op_mem(dHt, dHt, ptr[addr_diff_states_tp1_l_reg]);
            }

            // compute dCt
            const auto tmp_dCt1 = get_next_tmp_vmm();
            const auto tmp_dCt2 = get_next_tmp_vmm();

            uni_vmovups(tmp_dCt1, one_vmm);
            uni_vmovups(tmp_dCt2, tanhCt);
            uni_vfnmadd231ps(tmp_dCt1, tmp_dCt2, tmp_dCt2);
            uni_vmulps(tmp_dCt1, tmp_dCt1, dHt);
            to_float<src_data_t>(dG3, wg_addr(3), vlen_);
            uni_vmulps(tmp_dCt1, tmp_dCt1, dG3);
            uni_vmovups(dCt, ptr[addr_diff_c_states_tp1_l_reg]);
            uni_vaddps(dCt, dCt, tmp_dCt1);

            // compute dG3
            const auto tmp_dG3 = get_next_tmp_vmm();
            uni_vmovups(tmp_dG3, dG3);
            uni_vfnmadd231ps(dG3, tmp_dG3, tmp_dG3);
            uni_vmulps(dG3, dG3, dHt);
            uni_vmulps(dG3, dG3, tanhCt);

            // update dCt if lstm_peephole
            if (rnn_.is_lstm_peephole)
                vfmadd231ps_rhs_op_mem(dCt, dG3, weights_peephole_addr(2));

            // compute dG0
            // we will reuse G0 and G2 later for dG2
            to_float<src_data_t>(G0, wg_addr(0), vlen_);
            to_float<src_data_t>(dG2, wg_addr(2), vlen_);
            uni_vmovups(dG0, G0);
            uni_vfnmadd231ps(dG0, G0, G0);
            uni_vmulps(dG0, dG0, dCt);
            uni_vmulps(dG0, dG0, dG2);

            // compute dG1
            to_float<src_data_t>(G1, wg_addr(1), vlen_);
            uni_vmovups(dG1, G1);
            uni_vfnmadd231ps(dG1, G1, G1);
            uni_vmulps(dG1, dG1, dCt);
            vmulps_rhs_op_mem(dG1, dG1, ptr[addr_c_states_tm1_l_reg]);

            // compute dG2
            const auto tmp_dg2 = get_next_tmp_vmm();
            uni_vmovups(tmp_dg2, one_vmm);
            uni_vfnmadd231ps(tmp_dg2, dG2, dG2);
            uni_vmulps(G0, G0, dCt);
            uni_vmulps(tmp_dg2, tmp_dg2, G0);
            uni_vmovups(dG2, tmp_dg2);

            // compute diff_state_t_l
            uni_vmulps(dCt, dCt, G1);
            if (rnn_.is_lstm_peephole) {
                vfmadd231ps_rhs_op_mem(dCt, dG0, weights_peephole_addr(0));
                vfmadd231ps_rhs_op_mem(dCt, dG1, weights_peephole_addr(1));
            }
            uni_vmovups(ptr[addr_diff_c_states_t_l_reg], dCt);

            to_src<scratch_data_t>(sg_addr(0), dG0, vlen_);
            to_src<scratch_data_t>(sg_addr(1), dG1, vlen_);
            to_src<scratch_data_t>(sg_addr(2), dG2, vlen_);
            to_src<scratch_data_t>(sg_addr(3), dG3, vlen_);

            // increment address pointers
            add(addr_ws_gates_reg, vlen_scratch_);
            add(addr_scratch_gates_reg, vlen_scratch_);
            add(addr_diff_states_t_lp1_reg, vlen_);
            add(addr_diff_states_tp1_l_reg, vlen_);
            add(addr_diff_c_states_t_l_reg, vlen_);
            add(addr_diff_c_states_tp1_l_reg, vlen_);
            add(addr_c_states_tm1_l_reg, vlen_);
            add(addr_c_states_t_l_reg, vlen_);
            if (rnn_.is_lstm_peephole) add(addr_weights_peephole_reg, vlen_);
            inc_regs(vlen_);

            // increment loop counter
            sub(loop_cnt, vlen_scratch_);
            cmp(loop_cnt, vlen_scratch_);
            jge(vector_loop_start_label);
        }
        L(vector_loop_end_label);

        cmp(loop_cnt, 0);
        je(rem_loop_end_label, Xbyak::CodeGenerator::T_NEAR);
        // Same code as above, we just use vmovss for accessing inputs
        L(rem_loop_start_label);
        {
            const Xmm dG0(dG0_idx), dG1(dG1_idx), dG2(dG2_idx), dG3(dG3_idx),
                    tanhCt(tanhCt_idx), dHt(dHt_idx), dCt(dCt_idx), G0(G0_idx),
                    G1(G1_idx);

            // compute tanhCt
            uni_vmovss(tanhCt, ptr[addr_c_states_t_l_reg]);
            tanh_injector_->compute_vector(tanhCt.getIdx());

            // compute dHt
            // assumption: the diff_states_t_lp1 address is already offset by rnn.n_states
            uni_vmovss(dHt, ptr[addr_diff_states_t_lp1_reg]);
            if (!rnn_.is_lstm_projection)
                vaddss_rhs_op_mem(dHt, dHt, ptr[addr_diff_states_tp1_l_reg]);

            // compute dCt
            const auto tmp_dCt1 = get_next_tmp_xmm();
            const auto tmp_dCt2 = get_next_tmp_xmm();

            uni_vmovss(tmp_dCt1, one_xmm);
            // This overrides tanhCt when using Xmm
            uni_vmovss(tmp_dCt2, tanhCt);
            uni_vfnmadd231ss(tmp_dCt1, tmp_dCt2, tmp_dCt2);
            uni_vmulss(tmp_dCt1, tmp_dCt1, dHt);
            to_float<src_data_t>(dG3, wg_addr(3), hstate_dt_size_);
            uni_vmulss(tmp_dCt1, tmp_dCt1, dG3);
            uni_vmovss(dCt, ptr[addr_diff_c_states_tp1_l_reg]);
            uni_vaddss(dCt, dCt, tmp_dCt1);

            // compute dG3
            const auto tmp_dG3 = get_next_tmp_xmm();
            uni_vmovss(tmp_dG3, dG3);
            uni_vfnmadd231ss(dG3, tmp_dG3, tmp_dG3);
            uni_vmulss(dG3, dG3, dHt);
            uni_vmulss(dG3, dG3, tanhCt);

            // update dCt if lstm_peephole
            if (rnn_.is_lstm_peephole) {
                vfmadd231ss_rhs_op_mem(dCt, dG3, weights_peephole_addr(2));
            }

            // compute dG0
            // we will reuse G0 and G2 later for dG2
            to_float<src_data_t>(G0, wg_addr(0), hstate_dt_size_);
            to_float<src_data_t>(dG2, wg_addr(2), hstate_dt_size_);

            uni_vmovss(dG0, G0);
            uni_vfnmadd231ss(dG0, G0, G0);
            uni_vmulss(dG0, dG0, dCt);
            uni_vmulss(dG0, dG0, dG2);

            // compute dG1
            to_float<src_data_t>(G1, wg_addr(1), hstate_dt_size_);
            uni_vmovss(dG1, G1);
            uni_vfnmadd231ss(dG1, G1, G1);
            uni_vmulss(dG1, dG1, dCt);
            vmulss_rhs_op_mem(dG1, dG1, ptr[addr_c_states_tm1_l_reg]);

            // compute dG2
            const auto tmp_dG2 = get_next_tmp_xmm();
            uni_vmovss(tmp_dG2, one_xmm);
            uni_vfnmadd231ss(tmp_dG2, dG2, dG2);
            uni_vmulss(G0, G0, dCt);
            uni_vmulss(tmp_dG2, tmp_dG2, G0);
            uni_vmovss(dG2, tmp_dG2);

            // compute diff_state_t_l
            uni_vmulss(dCt, dCt, G1);
            if (rnn_.is_lstm_peephole) {
                vfmadd231ss_rhs_op_mem(dCt, dG1, weights_peephole_addr(1));
                vfmadd231ss_rhs_op_mem(dCt, dG0, weights_peephole_addr(0));
            }
            uni_vmovss(ptr[addr_diff_c_states_t_l_reg], dCt);

            to_src<scratch_data_t>(sg_addr(0), dG0, hstate_dt_size_);
            to_src<scratch_data_t>(sg_addr(1), dG1, hstate_dt_size_);
            to_src<scratch_data_t>(sg_addr(2), dG2, hstate_dt_size_);
            to_src<scratch_data_t>(sg_addr(3), dG3, hstate_dt_size_);

            // increment address pointers
            add(addr_ws_gates_reg, scratch_dt_size_);
            add(addr_scratch_gates_reg, scratch_dt_size_);
            add(addr_diff_states_t_lp1_reg, hstate_dt_size_);
            add(addr_diff_states_tp1_l_reg, hstate_dt_size_);
            add(addr_diff_c_states_t_l_reg, cstate_dt_size_);
            add(addr_diff_c_states_tp1_l_reg, cstate_dt_size_);
            add(addr_c_states_tm1_l_reg, cstate_dt_size_);
            add(addr_c_states_t_l_reg, cstate_dt_size_);
            if (rnn_.is_lstm_peephole)
                add(addr_weights_peephole_reg, weights_peephole_dt_size_);
            inc_regs(hstate_dt_size_);

            // increment loop counter
            sub(loop_cnt, scratch_dt_size_);
            cmp(loop_cnt, 0);
            jg(rem_loop_start_label);
        }
        L(rem_loop_end_label);

        postamble();

        tanh_injector_->prepare_table();
        init_table(vlen_);
        L(table_label);
        {
            for (size_t i = 0; i < vlen_ / sizeof(float); ++i)
                dd(float2int(1.0f));
        }
    }
};

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif
