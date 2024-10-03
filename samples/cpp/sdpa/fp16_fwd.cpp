/*
 * Copyright (c) 2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <catch2/catch_test_macros.hpp>
#include "../../utils/helpers.h"

#include <cuda_runtime_api.h>

#include <cudnn_frontend.h>
namespace fe = cudnn_frontend;

/*
Run this example by using command:
bin/samples "Toy sdpa forward"

This example shows how to construct a sdpa forward graph.
*/

// Tensors in forward pass
#define Q_UID 1
#define K_UID 2
#define V_UID 3
#define O_UID 4
#define STATS_UID 5
#define BIAS_UID 6
#define SEQ_LEN_Q_UID 7
#define SEQ_LEN_KV_UID 8

std::shared_ptr<fe::graph::Graph>
create_sdpa_forward_graph(int64_t const b,
                          int64_t const h_q,
                          int64_t const h_k,
                          int64_t const h_v,
                          int64_t const s_q,
                          int64_t const s_kv,
                          int64_t const d_qk,
                          int64_t const d_v,
                          float const attn_scale  = 1.0f,
                          bool const is_inference = false,
                          bool const causal_mask  = false,
                          bool const alibi_mask   = false,
                          bool const padding_mask = false,
                          bool has_attn_bias      = false) {
    // Create a graph and set common global properties.
    auto graph = std::make_shared<fe::graph::Graph>();
    graph->set_io_data_type(fe::DataType_t::BFLOAT16)
        .set_intermediate_data_type(fe::DataType_t::FLOAT)
        .set_compute_data_type(fe::DataType_t::FLOAT);

    auto Q = graph->tensor(fe::graph::Tensor_attributes()
                               .set_name("Q")
                               .set_uid(Q_UID)
                               .set_dim({b, h_q, s_q, d_qk})
                               .set_stride({h_q * s_q * d_qk, s_q * d_qk, d_qk, 1}));

    auto K = graph->tensor(fe::graph::Tensor_attributes()
                               .set_name("K")
                               .set_uid(K_UID)
                               .set_dim({b, h_k, s_kv, d_qk})
                               .set_stride({h_k * s_kv * d_qk, s_kv * d_qk, d_qk, 1}));

    auto V = graph->tensor(fe::graph::Tensor_attributes()
                               .set_name("V")
                               .set_uid(V_UID)
                               .set_dim({b, h_v, s_kv, d_v})
                               .set_stride({h_v * s_kv * d_v, s_kv * d_v, d_v, 1}));

    auto sdpa_options = fe::graph::SDPA_attributes()
                            .set_name("flash_attention")
                            .set_is_inference(is_inference)
                            .set_alibi_mask(alibi_mask)
                            .set_causal_mask(causal_mask)
                            .set_attn_scale(attn_scale);

    if (has_attn_bias) {
        auto bias = graph->tensor(fe::graph::Tensor_attributes()
                                      .set_name("bias")
                                      .set_uid(BIAS_UID)
                                      .set_dim({b, 1, s_q, s_kv})
                                      .set_stride({s_q * s_kv, s_q * s_kv, s_kv, 1}));
        sdpa_options.set_bias(bias);
    }

    if (padding_mask) {
        auto seq_q  = graph->tensor(fe::graph::Tensor_attributes()
                                       .set_name("seq_q")
                                       .set_uid(SEQ_LEN_Q_UID)
                                       .set_dim({b, 1, 1, 1})
                                       .set_stride({1, 1, 1, 1})
                                       .set_data_type(fe::DataType_t::INT32));
        auto seq_kv = graph->tensor(fe::graph::Tensor_attributes()
                                        .set_name("seq_kv")
                                        .set_uid(SEQ_LEN_KV_UID)
                                        .set_dim({b, 1, 1, 1})
                                        .set_stride({1, 1, 1, 1})
                                        .set_data_type(fe::DataType_t::INT32));
        sdpa_options.set_padding_mask(padding_mask).set_seq_len_q(seq_q).set_seq_len_kv(seq_kv);
    }

    auto [O, Stats] = graph->sdpa(Q, K, V, sdpa_options);

    O->set_output(true).set_dim({b, h_q, s_q, d_v}).set_stride({h_q * d_v, d_v, b * h_q * d_v, 1}).set_uid(O_UID);

    if (is_inference) {
        assert(Stats == nullptr);
    } else {
        Stats->set_output(true).set_data_type(fe::DataType_t::FLOAT).set_uid(STATS_UID);
    }

    return graph;
}

using namespace cudnn_frontend::graph;
void gpu_float_sdpa()
{
    int64_t b          = 1;     // batch size 3
    int64_t h_q        = 16;     // head dim
    int64_t h_k        = 16;     // head dim
    int64_t h_v        = 16;     // head dim
    int64_t s_q        = 384;  // q tensor is padded to this seq length 1024
    int64_t s_kv       = 384;  // k and v tensor is padded to this seq length 1024
    int64_t d_qk       = 64;   // hidden dim 64
    int64_t d_v        = 64;   // hidden dim 64
    bool is_inference  = false;
    float attn_scale   = 0.123f;
    bool causal_mask   = false; // true
    bool padding_mask  = false; // (cudnnGetVersion() >= 8903);
    bool alibi_mask    = false; // (cudnnGetVersion() >= 8904);
    bool has_attn_bias = false; // (cudnnGetVersion() >= 8903);

    if (cudnnGetVersion() < 8903) {
        SKIP("Test requires cudnn 8.9.3 or above");
        return;
    }
    std::cout<<"cudnn version: "<<cudnnGetVersion()<<std::endl;

    cudnnHandle_t handle;
    checkCudnnErr(cudnnCreate(&handle));
    create_sdpa_forward_graph_timer.start();
    auto graph = create_sdpa_forward_graph(b,
                                           h_q,
                                           h_k,
                                           h_v,
                                           s_q,
                                           s_kv,
                                           d_qk,
                                           d_v,
                                           attn_scale,
                                           is_inference,
                                           causal_mask,
                                           alibi_mask,
                                           padding_mask,
                                           has_attn_bias);
    create_sdpa_forward_graph_timer.stop();
    REQUIRE(graph->build(handle, {fe::HeurMode_t::A}).is_good());

    //// Build variant pack
    Surface<half> q_tensor(b * h_q * s_q * d_qk, false);
    Surface<half> k_tensor(b * h_k * d_qk * s_kv, false);
    Surface<half> v_tensor(b * h_v * d_v * s_kv, false);

    Surface<half> o_tensor(b * s_q * h_q * d_qk, false);

    std::unordered_map<fe::graph::Tensor_attributes::uid_t, void*> variant_pack = {
        {Q_UID, q_tensor.devPtr}, {K_UID, k_tensor.devPtr}, {V_UID, v_tensor.devPtr}, {O_UID, o_tensor.devPtr}};

    Surface<half> bias_tensor(b * 1 * s_q * s_kv, false);
    if (has_attn_bias) {
        variant_pack[BIAS_UID] = bias_tensor.devPtr;
    }

    Surface<int32_t> devActualSeqlenQ(b, false);
    Surface<int32_t> devActualSeqlenKV(b, false);
    if (padding_mask) {
        std::vector<int32_t> hostActualSeqlenQ(b, 20);
        std::vector<int32_t> hostActualSeqlenKV(b, 20);

        checkCudaErr(cudaMemcpy(devActualSeqlenQ.devPtr,
                                hostActualSeqlenQ.data(),
                                sizeof(hostActualSeqlenQ[0]) * b,
                                cudaMemcpyHostToDevice));
        checkCudaErr(cudaMemcpy(devActualSeqlenKV.devPtr,
                                hostActualSeqlenKV.data(),
                                sizeof(hostActualSeqlenKV[0]) * b,
                                cudaMemcpyHostToDevice));
        checkCudaErr(cudaDeviceSynchronize());

        variant_pack[SEQ_LEN_Q_UID]  = devActualSeqlenQ.devPtr;
        variant_pack[SEQ_LEN_KV_UID] = devActualSeqlenKV.devPtr;
    }

    Surface<float> statsTensor(b * h_q * s_q * 1, false);
    if (is_inference == false) {
        variant_pack[STATS_UID] = statsTensor.devPtr;
    }

    Surface<int8_t> workspace(graph->get_workspace_size(), false);
    execution_timer.start();
    REQUIRE(graph->execute(handle, variant_pack, workspace.devPtr).is_good());
    execution_timer.stop();
    checkCudaErr(cudaDeviceSynchronize());

    cudnnDestroy(handle);

}

TEST_CASE("Toy sdpa forward", "[graph][sdpa][flash][forward]") {
    std::cout<<"fp16 fwd start"<<std::endl;
        size_t fixed_run_times = 1; //1000
        size_t warmup_run_times = 0; //5

    for (size_t iter = 0; iter < warmup_run_times + fixed_run_times; ++iter) {
        if (iter == warmup_run_times) {
            create_sdpa_forward_graph_timer.reset(); 
            validate_timer.reset();
            build_operation_graph_timer.reset();
            create_execution_plans_timer.reset();
            check_support_timer.reset();
            build_plans_timer.reset();
            execution_timer.reset();
        }
        gpu_float_sdpa();
    }
    std::cout << "perf summary:" << std::endl;
    double total_time = create_sdpa_forward_graph_timer.avg()
            + build_operation_graph_timer.avg() + create_execution_plans_timer.avg()
            + build_plans_timer.avg() + execution_timer.avg();
    std::cout << "create_sdpa_forward_graph_timer:" << create_sdpa_forward_graph_timer.avg()
              << " ms, percentage of total time: "
              << create_sdpa_forward_graph_timer.avg() / total_time << std::endl;
    // std::cout << "validate_timer:" << validate_timer.avg()
    //           << " ms, percentage of total time: "
    //           << validate_timer.avg() / total_time << std::endl;
    std::cout << "build_operation_graph_timer:" << build_operation_graph_timer.avg()
              << " ms, percentage of total time: "
              << build_operation_graph_timer.avg() / total_time << std::endl;
    std::cout << "create_execution_plans_timer:" << create_execution_plans_timer.avg()
              << " ms, percentage of total time: "
              << create_execution_plans_timer.avg() / total_time << std::endl;
    // std::cout << "check_support_timer:" << check_support_timer.avg()
    //           << " ms, percentage of total time: "
    //           << check_support_timer.avg() / total_time << std::endl;
    std::cout << "build_plans_timer:" << build_plans_timer.avg()
              << " ms, percentage of total time: "
              << build_plans_timer.avg() / total_time << std::endl;
    std::cout << "execution_timer:" << execution_timer.avg()
              << " ms, percentage of total time: "
              << execution_timer.avg() / total_time << std::endl;

    std::cout<<"fp16 fwd end"<<std::endl;
}
