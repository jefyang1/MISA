/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2020-2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include "perf.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <initializer_list>
#include <cstdint>
#include <tuple>
#include <algorithm>
#include <iterator>
#include <functional>
#include <thread>
#include <sstream>

#define DO_SERIALIZE_TO_FILE

using index_t = uint64_t;

typedef struct {
    index_t tid;
    index_t data_byte;      // 1, 2, 4
    index_t vector;         // x1, x2, x4...
    index_t offset;         // start offset of this request, in byte
    bool valid;             // if this request is valid, aka, within tensor range
} req_t;

typedef struct {
    index_t block_size;
    index_t req_idx;            // requst index, a counter for the request
    std::vector<index_t> bid;   // for A/B matrix there maybe multiple block loading the same address
    std::vector<req_t> req;     // request of each thread
} block_req_t;

void serialize_block_req(const block_req_t * block_req, FILE* fp, std::vector<bool> * record)
{
#ifdef DO_SERIALIZE_TO_FILE
    fprintf(fp, "[b:");
    for(auto b : block_req->bid)
        fprintf(fp, "%zu,", b);
    fprintf(fp, " r:%zu]", block_req->req_idx);
#endif
    assert(block_req->block_size == block_req->req.size());
    std::ostringstream ss;
    index_t num_pixel_total = 0;
    index_t num_pixel_valid = 0;
    for(int i=0; i<block_req->req.size(); i++){
        const auto & thread_req = block_req->req[i];
        assert(thread_req.tid == i);
        ss<<"t"<<i<<":";
        for(int v=0; v<thread_req.vector; v++){
            index_t offset = thread_req.offset + v * thread_req.data_byte;
            index_t ipixel = offset / thread_req.data_byte;
            // printf("bid:%zu, rid:%zu, tid:%zu, pixel:%zu, %s\n", block_req->bid, block_req->req_idx, i, ipixel, thread_req.valid ? "y":"n");
            if(record && thread_req.valid){
                if((*record)[ipixel]){
                    //printf("impossible, this pixel:%zu has beed visited before[tid:%zu, r:%zu]\n", ipixel, i, block_req->req_idx);
                    //assert(0);
                    // in some case(like stride=2 1x1 with padding) there still exist 2 different gemm_m may have overlap (corner case)
                }
                (*record)[ipixel] = true;
                num_pixel_valid++;
            }
            num_pixel_total++;
            ss<<std::hex<<offset<<std::dec;
            if(v != (thread_req.vector - 1))
                ss<<",";
        }
        ss<<"("<<( thread_req.valid ? "y":"n" ) << ")";
        if((i + 1) % 4 == 0)
            ss<< "\n";
        else
            ss << "\t";
    }
#ifdef DO_SERIALIZE_TO_FILE
    fprintf(fp, " access:%zu/%zu(%.1f%%)\n", num_pixel_valid, num_pixel_total, ((float)num_pixel_valid) / num_pixel_total * 100);
    fprintf(fp, "%s", ss.str().c_str());
    fprintf(fp, "----------------------------------------------------------------\n");
#endif
    fflush(fp);
}

class linear_tensor_t{
public:
    linear_tensor_t(std::initializer_list<index_t> _dims):dims(_dims){}

    // get nd indices from a linear index
    std::vector<index_t> get(index_t linear_index) const
    {
        std::vector<index_t> nd_index(dims.size(), (index_t)0);
        index_t len = 1;
        auto  rind_itr = std::rbegin(nd_index);
        for(auto  rdim_itr = dims.rbegin();
                    rdim_itr != dims.rend();
                    rdim_itr++, rind_itr++){
            *rind_itr = (linear_index / len) % *rdim_itr;
            len *= *rdim_itr;
        }

        return nd_index;
    }
    // get offset from nd indices
    index_t offset(std::initializer_list<index_t> indices) const
    {
        assert(indices.size() == dims.size());
        index_t stride = 1;
        index_t len = 0;
        auto rind_itr = std::rbegin(indices);
        for(auto  rdim_itr = dims.rbegin();
                    rdim_itr != dims.rend();
                    rdim_itr++, rind_itr++){
            len += *rind_itr * stride;
            stride *= *rdim_itr;
        }
        return len;
    }

    // nd range check
    bool range_check(std::initializer_list<index_t> indices) const
    {
        assert(indices.size() == dims.size());
        bool valid = true;
        auto rind_itr = std::rbegin(indices);
        for(auto  rdim_itr = dims.rbegin();
                    rdim_itr != dims.rend();
                    rdim_itr++, rind_itr++){
            valid &= *rind_itr < *rdim_itr;
        }
        return valid;
    }
    index_t size() const
    {
        return std::accumulate(dims.begin(), dims.end(), 1, std::multiplies<index_t>());
    }
private:
    std::vector<index_t> dims;
};

static inline index_t gmap_conv_out_size(index_t in_size, index_t pad, index_t dilation,
                                   index_t ksize, index_t stride) {
    return (in_size + 2 * pad - dilation * (ksize - 1) - 1) / stride + 1;
}

static inline std::tuple<std::vector<bool>, std::vector<bool>>
gmap_get_input_access_map(const args_t *conv_args)
{
    // in convolution, sometime the input pixel may not all be used.
    // this function return 2 map indicate which h/w is used
    index_t hi = conv_args->get_int("in_h");
    index_t wi = conv_args->get_int("in_w");
    index_t n = conv_args->get_int("batchsize");
    index_t k = conv_args->get_int("out_channels");
    index_t c = conv_args->get_int("in_channels");

    index_t stride_h = conv_args->get_int("conv_stride_h");
    index_t stride_w = conv_args->get_int("conv_stride_w");
    index_t dilation_h = conv_args->get_int("dilation_h");
    index_t dilation_w = conv_args->get_int("dilation_w");
    index_t pad_h = conv_args->get_int("pad_h");
    index_t pad_w = conv_args->get_int("pad_w");
    index_t y = conv_args->get_int("fil_h");
    index_t x = conv_args->get_int("fil_w");
    index_t ho = gmap_conv_out_size(hi, pad_h, dilation_h, y, stride_h);
    index_t wo = gmap_conv_out_size(wi, pad_w, dilation_w, x, stride_w);
    index_t group = conv_args->get_int("group_count");

    std::vector<bool> valid_hi(hi, false);
    std::vector<bool> valid_wi(wi, false);

    for (index_t iho = 0; iho < ho; iho++){
        for(index_t iy = 0; iy < y; iy++){
            index_t ihi = stride_h * iho - pad_h + dilation_h * iy;
            if(ihi < hi)
                valid_hi[ihi] = true;
        }
    }

    for (index_t iwo = 0; iwo < wo; iwo++){
        for(index_t ix = 0; ix < x; ix++){
            index_t iwi = stride_w * iwo - pad_w + dilation_w * ix;
            if(iwi < wi)
                valid_wi[iwi] = true;
        }
    }

    return std::make_tuple(valid_hi, valid_wi);
}

#define GMAP_DIR "gmap/"

std::tuple<std::string, std::string, std::string>
gmap_get_dump_file_name(const std::string base_dir, const igemm_gtc_tunable_t * tunable)
{
    std::string kernel_name = igemm_gtc_encode_kernel_name(tunable);
    return std::make_tuple(base_dir + "/" + std::string("gmap_") + kernel_name +std::string("_inp.dump"),
                           base_dir + "/" + std::string("gmap_") + kernel_name +std::string("_wei.dump"),
                           base_dir + "/" + std::string("gmap_") + kernel_name +std::string("_out.dump"));
}

void gmap_dump_fwd_nhwc(const args_t *conv_args, const igemm_gtc_tunable_t * tunable, int gks, FILE *fp_inp, FILE *fp_wei, FILE *fp_out)
{
    index_t hi = conv_args->get_int("in_h");
    index_t wi = conv_args->get_int("in_w");
    index_t n = conv_args->get_int("batchsize");
    index_t k = conv_args->get_int("out_channels");
    index_t c = conv_args->get_int("in_channels");

    index_t stride_h = conv_args->get_int("conv_stride_h");
    index_t stride_w = conv_args->get_int("conv_stride_w");
    index_t dilation_h = conv_args->get_int("dilation_h");
    index_t dilation_w = conv_args->get_int("dilation_w");
    index_t pad_h = conv_args->get_int("pad_h");
    index_t pad_w = conv_args->get_int("pad_w");
    index_t y = conv_args->get_int("fil_h");
    index_t x = conv_args->get_int("fil_w");
    index_t ho = gmap_conv_out_size(hi, pad_h, dilation_h, y, stride_h);
    index_t wo = gmap_conv_out_size(wi, pad_w, dilation_w, x, stride_w);
    index_t group = conv_args->get_int("group_count");

    std::string precision = tunable->precision;
    index_t data_byte = utility_string_to_data_byte(tunable->precision);

    index_t num_global_splits = tunable->gemm_k_global_split ? (1 << gks) : 1;
    index_t gemm_m_per_block = tunable->gemm_m_per_block;
    index_t gemm_n_per_block = tunable->gemm_n_per_block;
    index_t gemm_k_per_block = tunable->gemm_k_per_block;
    index_t gemm_m = ((n * ho * wo + gemm_m_per_block - 1) / gemm_m_per_block) * gemm_m_per_block;
    index_t gemm_n = ((k / group) + gemm_n_per_block - 1) / gemm_n_per_block * gemm_n_per_block;
    index_t gemm_k = tunable->merge_e ? ((c / group) * y * x + gemm_k_per_block - 1) / gemm_k_per_block * gemm_k_per_block :
                                        ((c / group) * y * x) / num_global_splits;

    index_t ta_e    = tunable->tensor_a_thread_lengths[0];
    index_t ta_c    = tunable->tensor_a_thread_lengths[1];
    index_t ta_nb0  = tunable->tensor_a_thread_lengths[2];
    index_t ta_nb1  = tunable->tensor_a_thread_lengths[3];

    index_t tb_e    = tunable->tensor_b_thread_lengths[0];
    index_t tb_c    = tunable->tensor_b_thread_lengths[1];
    index_t tb_k0   = tunable->tensor_b_thread_lengths[2];
    index_t tb_k1   = tunable->tensor_b_thread_lengths[3];

    index_t ca_e    = tunable->tensor_a_cluster_lengths[0];
    index_t ca_c    = tunable->tensor_a_cluster_lengths[1];
    index_t ca_nb0  = tunable->tensor_a_cluster_lengths[2];
    index_t ca_nb1  = tunable->tensor_a_cluster_lengths[3];

    index_t cb_e    = tunable->tensor_b_cluster_lengths[0];
    index_t cb_c    = tunable->tensor_b_cluster_lengths[1];
    index_t cb_k0   = tunable->tensor_b_cluster_lengths[2];
    index_t cb_k1   = tunable->tensor_b_cluster_lengths[3];

    if(tunable->merge_e)
        assert(ta_c == 1 && tb_c == 1); // currently only support this

    // printf("ta%dx%dx%dx%d_%dx%dx%dx%d, tb%dx%dx%dx%d_%dx%dx%dx%d\n",
    //             ta_e, ta_c, ta_nb0, ta_nb1, ca_e, ca_c, ca_nb0, ca_nb1,
    //             tb_e, tb_c, tb_k0 , tb_k1 , cb_e, cb_c, cb_k0 , cb_k1 );

    index_t block_size = ca_e * ca_c * ca_nb0 * ca_nb1;
    assert(block_size == (cb_e * cb_c * cb_k0 * cb_k1));
    assert((gemm_m % gemm_m_per_block == 0) && (gemm_n % gemm_n_per_block == 0));
    index_t grid_size = group * num_global_splits * (gemm_m / gemm_m_per_block) * (gemm_n / gemm_n_per_block);
    linear_tensor_t block_mapping({group, num_global_splits, (gemm_m / gemm_m_per_block), (gemm_n / gemm_n_per_block)});
    linear_tensor_t gemm_m_transform({n, ho, wo});
    linear_tensor_t gemm_k_transform({y, x, c / group});

    linear_tensor_t tensor_inp({n, hi, wi, group, c/group});
    linear_tensor_t tensor_wei({group, k/group, y, x, c/group});
    linear_tensor_t tensor_out({n, ho, wo, group, k/group});
    std::vector<bool> record_inp(n*hi*wi*(c/group), false);
    std::vector<bool> record_wei(group*(k/group)*y*x*(c/group), false);
    std::vector<bool> record_out(n*ho*wo*(k/group), false);

    index_t ta_nb_per_thread = ta_nb0 != 1 ? ta_nb0 : ta_nb1;
    index_t ta_vector_c = utility_gcd(ta_c, 4 * (4 / data_byte));
    index_t ta_nc_per_thread = ta_c / ta_vector_c;
    index_t ta_nb_thread_stride = tunable->tensor_a_pass_through ? ca_nb0 * ca_nb1 : (
                                    ta_nb0 != 1 ? ca_nb1 * ta_nb1 :1);
    
    index_t tb_nk_per_thread = tb_k0 != 1 ? tb_k0 : tb_k1;
    index_t tb_vector_c = utility_gcd(tb_c, 4 * (4 / data_byte));
    index_t tb_nc_per_thread = tb_c / tb_vector_c;
    index_t tb_nk_thread_stride = tb_k0 != 1 ? cb_k1 * tb_k1 : 1;

    // check get_vector_write_out()
    index_t tc_vector_k = 1;    // 1 when fp32
    if(tunable->precision == "fp16"){
        if(tunable->gemm_k_global_split)
            tc_vector_k = 2;
        else{
            if(ta_c == 1 && tb_c == 1)
                tc_vector_k = 1;
            else
                tc_vector_k = utility_gcd(gemm_n_per_block, static_cast<index_t>(tunable->vector_store == 0 ? 8 : tunable->vector_store));
        }
    }
    else if(tunable->precision == "int8")
    {
        if(ta_c == 1 && tb_c == 1)
            tc_vector_k = 1;
        else
            tc_vector_k = utility_gcd(gemm_n_per_block, static_cast<index_t>(tunable->vector_store == 0 ? 16 : tunable->vector_store));
    }

    assert(gemm_n_per_block % tc_vector_k == 0);
    index_t cc_k = gemm_n_per_block / tc_vector_k;
    assert(block_size % cc_k == 0);
    index_t cc_nb = block_size / cc_k;
    assert(gemm_m_per_block % cc_nb == 0);
    index_t tc_nb_per_thread = gemm_m_per_block / cc_nb;
    index_t tc_nb_thread_stride = cc_nb;

    std::vector<index_t> ta_block_req_idx(grid_size, 0);
    std::vector<index_t> tb_block_req_idx(grid_size, 0);
    std::vector<index_t> tc_block_req_idx(grid_size, 0);

    std::vector<block_req_t> inp_block_req;
    linear_tensor_t inp_block_req_desc({num_global_splits, gemm_m / gemm_m_per_block, gemm_k / gemm_k_per_block, ta_nb_per_thread, ta_nc_per_thread});
    inp_block_req.resize(inp_block_req_desc.size());

    std::vector<block_req_t> wei_block_req;
    linear_tensor_t wei_block_req_desc({num_global_splits, gemm_n / gemm_n_per_block, gemm_k / gemm_k_per_block, tb_nk_per_thread, tb_nc_per_thread});
    wei_block_req.resize(wei_block_req_desc.size());

    std::vector<block_req_t> out_block_req;
    linear_tensor_t out_block_req_desc({gemm_m / gemm_m_per_block, gemm_n / gemm_n_per_block, tc_nb_per_thread});
    out_block_req.resize(out_block_req_desc.size());

    auto cur_block = [&](index_t bid, index_t cur_group, index_t cur_gks, index_t cur_gemm_m, index_t cur_gemm_n, index_t cur_gemm_k){
        // inp
        auto cur_block_inp = [&](){
            for(index_t t_inb = 0; t_inb < ta_nb_per_thread; t_inb++){
                for(index_t t_ic = 0; t_ic < ta_nc_per_thread; t_ic++){
                    index_t i_b_req = inp_block_req_desc.offset({cur_gks, cur_gemm_m / gemm_m_per_block, cur_gemm_k / gemm_k_per_block, t_inb, t_ic});
                    block_req_t & b_req = inp_block_req[i_b_req];
                    b_req.block_size = block_size;
                    b_req.bid.push_back(bid);
                    b_req.req_idx = ta_block_req_idx[bid];
                    ta_block_req_idx[bid]++;

                    // printf("bid:%zu, i_req:%zu(%zu,%zu), m:%zu, ta_nb_per_thread:%zu, ta_nc_per_thread:%zu\n",
                    //     bid, i_req,t_inb,t_ic, cur_gemm_m / gemm_m_per_block, ta_nb_per_thread, ta_nc_per_thread);

                    if(cur_gemm_n == 0){
                        for(index_t tid = 0; tid < block_size; tid++){
                            
                            index_t in_inb, in_ic;
                            if(tunable->tensor_a_pass_through){
                                index_t tmp = tid; index_t tmp1;
                                in_inb  = (tmp % ca_nb1) * ta_nb1; tmp /= ca_nb1;
                                in_ic   = (tmp % ca_c) * ta_vector_c; tmp /= ca_c;
                                tmp1    = (tmp % ca_nb0) * ta_nb0;
                                in_inb  = tmp1 * (ca_nb1 * ta_nb1) + in_inb;
                            }else{
                                in_ic   = (tid % ca_c) * ta_c;
                                in_inb  = (tid / ca_c) * ta_nb1;
                            }
                            index_t cur_in_inb = cur_gemm_m + in_inb + t_inb * ta_nb_thread_stride;

                            auto in_gemm_m_trans = gemm_m_transform.get(cur_in_inb);
                            auto in_gemm_k_trans = gemm_k_transform.get(cur_gemm_k + cur_gks * gemm_k + (tunable->merge_e ? in_ic : 0));

                            index_t cur_in_iy = in_gemm_k_trans[0];
                            index_t cur_in_ix = in_gemm_k_trans[1];
                            index_t cur_in_ic = in_gemm_k_trans[2] + (tunable->merge_e ? 0 : (in_ic + t_ic * ta_vector_c * (tunable->tensor_a_pass_through ? ca_c : 1)));

                            index_t cur_in_in = in_gemm_m_trans[0];
                            index_t cur_in_iho = in_gemm_m_trans[1];
                            index_t cur_in_iwo = in_gemm_m_trans[2];

                            // ihi = iho * s_stride_h + iy * s_dilation_h - s_pad_h
                            // iwi = iwo * s_stride_w + ix * s_dilation_w - s_pad_w
                            index_t cur_in_ihi = cur_in_iho * stride_h + cur_in_iy * dilation_h - pad_h;
                            index_t cur_in_iwi = cur_in_iwo * stride_w + cur_in_ix * dilation_w - pad_w;

                            auto cur_in_idx = {cur_in_in, cur_in_ihi, cur_in_iwi, cur_group, cur_in_ic};
                            bool cur_in_valid = tensor_inp.range_check(cur_in_idx);
                            index_t cur_in_offset = tensor_inp.offset(cur_in_idx) * data_byte;
                            b_req.req.emplace_back(req_t({tid, data_byte, ta_vector_c, cur_in_offset, cur_in_valid}));
                        }
                    }
                }
            }
        };
        auto cur_block_wei = [&](){
            for(index_t t_ik = 0; t_ik < tb_nk_per_thread; t_ik++){
                for(index_t t_ic = 0; t_ic < tb_nc_per_thread; t_ic++){
                    index_t i_b_req = wei_block_req_desc.offset({cur_gks, cur_gemm_n / gemm_n_per_block, cur_gemm_k / gemm_k_per_block, t_ik, t_ic});
                    block_req_t & b_req = wei_block_req[i_b_req];
                    b_req.block_size = block_size;
                    b_req.bid.push_back(bid);
                    b_req.req_idx = tb_block_req_idx[bid];
                    tb_block_req_idx[bid]++;

                    // printf("bid:%zu, i_req:%zu(%zu,%zu), m:%zu, ta_nb_per_thread:%zu, ta_nc_per_thread:%zu\n",
                    //     bid, i_req,t_ik,t_ic, cur_gemm_m / gemm_m_per_block, ta_nb_per_thread, ta_nc_per_thread);

                    if(cur_gemm_m == 0){
                        for(index_t tid = 0; tid < block_size; tid++){
                            index_t wei_ik, wei_ic;

                            wei_ic  = (tid % cb_c) * tb_c;
                            wei_ik  = (tid / cb_c) * tb_k1;

                            index_t cur_wei_ik = cur_gemm_n + wei_ik + t_ik * tb_nk_thread_stride;

                            auto wei_gemm_k_trans = gemm_k_transform.get(cur_gemm_k + cur_gks * gemm_k + (tunable->merge_e ? wei_ic : 0));

                            index_t cur_wei_iy = wei_gemm_k_trans[0];
                            index_t cur_wei_ix = wei_gemm_k_trans[1];
                            index_t cur_wei_ic = wei_gemm_k_trans[2] + (tunable->merge_e ? 0 : (wei_ic + t_ic * tb_vector_c));

                            auto cur_wei_idx = {cur_group, cur_wei_ik, cur_wei_iy, cur_wei_ix, cur_wei_ic};
                            bool cur_wei_valid = tensor_wei.range_check(cur_wei_idx);

                            index_t cur_wei_offset = tensor_wei.offset(cur_wei_idx) * data_byte;
                            b_req.req.emplace_back(req_t({tid, data_byte, tb_vector_c, cur_wei_offset, cur_wei_valid}));
                        }
                    }
                }
            }
        };

        auto cur_block_out = [&](){
            if(cur_gemm_k == 0){
                for(index_t t_inb = 0 ; t_inb < tc_nb_per_thread; t_inb++){
                    index_t i_b_req = out_block_req_desc.offset({cur_gemm_m / gemm_m_per_block, cur_gemm_n / gemm_n_per_block, t_inb});
                    block_req_t & b_req = out_block_req[i_b_req];
                    b_req.block_size = block_size;
                    b_req.bid.push_back(bid);
                    b_req.req_idx = tc_block_req_idx[bid];
                    tc_block_req_idx[bid]++;

                    if(cur_gks == 0){
                        for(index_t tid = 0; tid < block_size; tid++){
                            index_t out_inb, out_ik;
                            out_ik = (tid % cc_k) * tc_vector_k;
                            out_inb = tid / cc_k;

                            index_t cur_out_ik = cur_gemm_n + out_ik;
                            index_t cur_out_inb = cur_gemm_m + out_inb + t_inb * tc_nb_thread_stride;

                            auto out_gemm_m_trans = gemm_m_transform.get(cur_out_inb);

                            index_t cur_out_in = out_gemm_m_trans[0];
                            index_t cur_out_iho = out_gemm_m_trans[1];
                            index_t cur_out_iwo = out_gemm_m_trans[2];

                            auto cur_out_idx = {cur_out_in, cur_out_iho, cur_out_iwo, cur_group, cur_out_ik};
                            auto cur_out_valid = tensor_out.range_check(cur_out_idx);

                            index_t cur_out_offset = tensor_out.offset(cur_out_idx) * data_byte;
                            b_req.req.emplace_back(req_t({tid, data_byte, tc_vector_k, cur_out_offset, cur_out_valid}));
                        }
                    }
                }
            }
        };

        cur_block_inp();
        cur_block_wei();
        cur_block_out();
    };

    for(index_t bid = 0; bid < grid_size; bid++){
        auto cur_block_position = block_mapping.get(bid);   // position of this block in ndim space
        auto cur_group  = cur_block_position[0];
        auto cur_gks    = cur_block_position[1];
        auto cur_gemm_m = cur_block_position[2] * gemm_m_per_block;
        auto cur_gemm_n = cur_block_position[3] * gemm_n_per_block;
        for(index_t cur_gemm_k = 0; cur_gemm_k < gemm_k; cur_gemm_k += gemm_k_per_block){
            cur_block(bid, cur_group, cur_gks, cur_gemm_m, cur_gemm_n, cur_gemm_k);
        }
    }

    // serialize block request
    for(auto itr_ibr = inp_block_req.begin(); itr_ibr != inp_block_req.end(); itr_ibr++)
        serialize_block_req(&(*itr_ibr), fp_inp, &record_inp);
    
    for(auto itr_ibr = wei_block_req.begin(); itr_ibr != wei_block_req.end(); itr_ibr++)
        serialize_block_req(&(*itr_ibr), fp_wei, &record_wei);
    
    for(auto itr_ibr = out_block_req.begin(); itr_ibr != out_block_req.end(); itr_ibr++)
        serialize_block_req(&(*itr_ibr), fp_out, &record_out);

    // valid all record
    std::vector<bool> valid_hi, valid_wi;
    std::tie(valid_hi, valid_wi) = gmap_get_input_access_map(conv_args);
    for(auto it = record_inp.begin(); it != record_inp.end(); it++){
        index_t idx = std::distance(record_inp.begin(), it);
        std::vector<index_t> inp_position = tensor_inp.get(idx);
        index_t ihi = inp_position[1];
        index_t iwi = inp_position[2];
        if(valid_hi[ihi] && valid_wi[iwi]){
            if(!(*it)){
                printf("WARNING! input not touched pixel at %zu\n", idx);
            }
        }
        else{
            if(*it){
                printf("WARNING! input touched unused pixel at %zu\n", idx);
            }
        }
    }

    for(auto it = record_wei.begin(); it != record_wei.end(); it++){
        index_t idx = std::distance(record_wei.begin(), it);
        if(!(*it)){
            printf("WARNING! weight not touched pixel at %zu\n", idx);
        }
    }

    for(auto it = record_out.begin(); it != record_out.end(); it++){
        index_t idx = std::distance(record_out.begin(), it);
        if(!(*it)){
            printf("WARNING! output not touched pixel at %zu\n", idx);
        }
    }
}

void gmap_dump_banner(const args_t *conv_args, const igemm_gtc_tunable_t * tunable, FILE *fp_inp, FILE *fp_wei, FILE *fp_out)
{
    index_t hi = conv_args->get_int("in_h");
    index_t wi = conv_args->get_int("in_w");
    index_t n = conv_args->get_int("batchsize");
    index_t k = conv_args->get_int("out_channels");
    index_t c = conv_args->get_int("in_channels");

    index_t stride_h = conv_args->get_int("conv_stride_h");
    index_t stride_w = conv_args->get_int("conv_stride_w");
    index_t dilation_h = conv_args->get_int("dilation_h");
    index_t dilation_w = conv_args->get_int("dilation_w");
    index_t pad_h = conv_args->get_int("pad_h");
    index_t pad_w = conv_args->get_int("pad_w");
    index_t y = conv_args->get_int("fil_h");
    index_t x = conv_args->get_int("fil_w");
    index_t ho = gmap_conv_out_size(hi, pad_h, dilation_h, y, stride_h);
    index_t wo = gmap_conv_out_size(wi, pad_w, dilation_w, x, stride_w);
    index_t group = conv_args->get_int("group_count");

    std::string precision = tunable->precision;
    index_t data_byte = utility_string_to_data_byte(tunable->precision);

    // input
    fprintf(fp_inp, "[inp] %s, %s, ", tunable->tensor_layout.c_str(), tunable->precision.c_str());
    if(tunable->tensor_layout == "nchw")
        fprintf(fp_inp, "n:%zu, c:%zu, h:%zu, w:%zu, g:%zu", n, c, hi, wi, group);
    else if(tunable->tensor_layout == "nhwc")
        fprintf(fp_inp, "n:%zu, h:%zu, w:%zu, c:%zu, g:%zu", n, hi, wi, c, group);
    fprintf(fp_inp, "\n");

    // wei
    fprintf(fp_wei, "[wei] %s, %s, ", tunable->tensor_layout.c_str(), tunable->precision.c_str());
    if(tunable->tensor_layout == "nchw")
        fprintf(fp_wei, "k:%zu, c:%zu, y:%zu, x:%zu, g:%zu", k, c, y, x, group);
    else if(tunable->tensor_layout == "nhwc")
        fprintf(fp_wei, "k:%zu, y:%zu, x:%zu, c:%zu, g:%zu", k, y, x, c, group);
    fprintf(fp_wei, "\n");

    // out
    fprintf(fp_out, "[inp] %s, %s, ", tunable->tensor_layout.c_str(), tunable->precision.c_str());
    if(tunable->tensor_layout == "nchw")
        fprintf(fp_out, "n:%zu, k:%zu, h:%zu, w:%zu, g:%zu", n, k, ho, wo, group);
    else if(tunable->tensor_layout == "nhwc")
        fprintf(fp_out, "n:%zu, h:%zu, w:%zu, k:%zu, g:%zu", n, ho, wo, k, group);
    fprintf(fp_out, "\n");
}

// global memory access pattern
void gmap_dump(const args_t *conv_args, const igemm_gtc_tunable_t * tunable, int gks)
{
    int err = mkdir(GMAP_DIR, 0775);
    if(err != 0){
        if(errno == EEXIST){
            // printf("WARNING: directory %s already exist. will dump into it anyway.\n", GMAP_DIR);
        }else{
            printf("[%d]%s: fail to creat directory\n", errno, strerror(errno));
            return ;
        }
    }

    std::string gmap_file_inp;
    std::string gmap_file_wei;
    std::string gmap_file_out;
    std::tie(gmap_file_inp, gmap_file_wei, gmap_file_out) = gmap_get_dump_file_name(GMAP_DIR, tunable);

    FILE * fp_inp = fopen(gmap_file_inp.c_str(), "w");
    if(!fp_inp){
        printf("[%d]%s: fail to open file %s\n", errno, strerror(errno), gmap_file_inp.c_str());
        return ;
    }

    FILE * fp_wei = fopen(gmap_file_wei.c_str(), "w");
    if(!fp_wei){
        printf("[%d]%s: fail to open file %s\n", errno, strerror(errno), gmap_file_wei.c_str());
        return ;
    }

    FILE * fp_out = fopen(gmap_file_out.c_str(), "w");
    if(!fp_out){
        printf("[%d]%s: fail to open file %s\n", errno, strerror(errno), gmap_file_out.c_str());
        return ;
    }

    gmap_dump_banner(conv_args, tunable, fp_inp, fp_wei, fp_out);

    std::string tensor_layout = tunable->tensor_layout;
    std::string precision = tunable->precision;
    std::string direction = tunable->direction;

    if(direction == "fwd"){
        if(tensor_layout == "nchw"){

        }else if(tensor_layout == "nhwc"){
            gmap_dump_fwd_nhwc(conv_args, tunable, gks, fp_inp, fp_wei, fp_out);
        }else{
            assert(0);
        }
    }

    fclose(fp_inp);
    fclose(fp_wei);
    fclose(fp_out);
}
