/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
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

#include <miopen/conv/invokers/impl_gemm.hpp>
#include <miopen/solver.hpp>
#include <miopen/handle.hpp>
#include <miopen/generic_search.hpp>
#include <miopen/hip_build_utils.hpp>
#include <miopen/solver/implicitgemm_util.hpp>

MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_HIP_FWD_V4R5_XDLOPS)

/* this fix is for fp16 xdlops vectorizable kernels due to followings, we may revisit this fix after
  compiler fix:
  1. compiler issues(25% impact)
  2. LDS write performance(75% impact)
*/
MIOPEN_DECLARE_ENV_VAR(
    MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_FWD_V4R5_XDLOPS_ADD_VECTOR_LOAD_GEMMN_TUNE_PARAM)

namespace miopen {
namespace solver {

static std::tuple<int, int, int, int> CalculateGemmSize(const ProblemDescription& problem)
{
    const std::size_t g  = ProblemInterpreter::GetGroupCountG(problem);
    const std::size_t n  = ProblemInterpreter::GetBatchN(problem);
    const std::size_t k  = ProblemInterpreter::GetOutputChannelK(problem);
    const std::size_t c  = ProblemInterpreter::GetInputChannelC(problem);
    const std::size_t ho = ProblemInterpreter::GetOutputHeightHo(problem);
    const std::size_t wo = ProblemInterpreter::GetOutputWidthWo(problem);
    const std::size_t y  = ProblemInterpreter::GetFilterHeightY(problem);
    const std::size_t x  = ProblemInterpreter::GetFilterWidthX(problem);

    const std::size_t k_per_group = k / g;
    const std::size_t c_per_group = c / g;

    const std::size_t gemm_g       = g;
    const std::size_t gemm_m       = k_per_group;
    const std::size_t gemm_n       = n * ho * wo;
    const std::size_t gemm_k_total = c_per_group * y * x;

    return std::make_tuple(gemm_g, gemm_m, gemm_n, gemm_k_total);
}

PerformanceImplicitGemmForwardV4R5Xdlops::PerformanceImplicitGemmForwardV4R5Xdlops()
    : PerformanceImplicitGemmForwardV4R5Xdlops::PerformanceImplicitGemmForwardV4R5Xdlops(
          4, 4, 1, 4, 4, 1, false, false, 1, false)
{
}

PerformanceImplicitGemmForwardV4R5Xdlops::PerformanceImplicitGemmForwardV4R5Xdlops(bool spare)
    : PerformanceImplicitGemmForwardV4R5Xdlops::PerformanceImplicitGemmForwardV4R5Xdlops(
          4, 4, 1, 4, 4, 1, false, false, 1, spare)
{
}

PerformanceImplicitGemmForwardV4R5Xdlops::PerformanceImplicitGemmForwardV4R5Xdlops(
    int GemmMPerBlock_,
    int GemmNPerBlock_,
    int GemmKPerBlock_,
    int GemmMPerWave_,
    int GemmNPerWave_,
    int GemmKPack_,
    bool GemmAThreadCopyMoreGemmK_,
    bool GemmBThreadCopyMoreGemmKPack_,
    int GemmBThreadDataPerRead_GemmN_,
    bool use_spare_set_)
    : GemmMPerBlock(GemmMPerBlock_),
      GemmNPerBlock(GemmNPerBlock_),
      GemmKPerBlock(GemmKPerBlock_),
      GemmMPerWave(GemmMPerWave_),
      GemmNPerWave(GemmNPerWave_),
      GemmKPack(GemmKPack_),
      GemmAThreadCopyMoreGemmK(GemmAThreadCopyMoreGemmK_),
      GemmBThreadCopyMoreGemmKPack(GemmBThreadCopyMoreGemmKPack_),
      GemmBThreadDataPerRead_GemmN(GemmBThreadDataPerRead_GemmN_),
      use_spare_set(use_spare_set_)
{
}

bool PerformanceImplicitGemmForwardV4R5Xdlops::operator==(
    const PerformanceImplicitGemmForwardV4R5Xdlops& other) const
{
    // clang-format off
    return GemmMPerBlock == other.GemmMPerBlock
        && GemmNPerBlock == other.GemmNPerBlock
        && GemmKPerBlock == other.GemmKPerBlock
        && GemmMPerWave == other.GemmMPerWave
        && GemmNPerWave == other.GemmNPerWave
        && GemmKPack == other.GemmKPack
        && GemmAThreadCopyMoreGemmK  == other.GemmAThreadCopyMoreGemmK
        && GemmBThreadCopyMoreGemmKPack  == other.GemmBThreadCopyMoreGemmKPack
        && GemmBThreadDataPerRead_GemmN  == other.GemmBThreadDataPerRead_GemmN
        && use_spare_set == other.use_spare_set;
    // clang-format on
}

bool PerformanceImplicitGemmForwardV4R5Xdlops::SetNextValue(const ConvolutionContext& /*ctx*/)
{
    do
    {
        // list performance parameters in reverse order, in order for tuning to iterate over the
        // range in normal order
        if(miopen::IsEnabled(
               MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_FWD_V4R5_XDLOPS_ADD_VECTOR_LOAD_GEMMN_TUNE_PARAM))
        {
            if(!NextTwoPower<1, 8>(GemmBThreadDataPerRead_GemmN))
                break;
        }
        if(!NextFlag<false, true>(GemmBThreadCopyMoreGemmKPack))
            break;
        // force to be false to reduce search space
        if(!NextFlag<false, false>(GemmAThreadCopyMoreGemmK))
            break;
        if(!NextTwoPower<1, 8>(GemmKPack))
            break;
        if(!NextTwoPower<4, 128>(GemmNPerWave))
            break;
        if(!NextTwoPower<4, 128>(GemmMPerWave))
            break;
        if(!NextTwoPower<1, 8>(GemmKPerBlock))
            break;
        if(!NextTwoPower<4, 256>(GemmNPerBlock))
            break;
        if(!NextTwoPower<4, 256>(GemmMPerBlock))
            break;
        return false;
    } while(false);

    return true;
}

void PerformanceImplicitGemmForwardV4R5Xdlops::HeuristicInit(const ConvolutionContext& ctx,
                                                             const ProblemDescription& problem)
{
    PerformanceImplicitGemmForwardV4R5Xdlops tmp;

    // loop over certain ranges of tuning parameter
    auto get_euristic_config = [&](auto is_valid_func) {
        if(problem.IsFp32())
        {
            tmp = {256, 256, 8, 128, 128, 4, false, true, 1};

            bool all_visited = false;
            do
            {
                do
                {
                    // list in reverse order of importance,
                    // and favor large GEMM
                    if(!PreviousTwoPower<1, 8>(tmp.GemmBThreadDataPerRead_GemmN))
                        break;
                    if(!PreviousTwoPower<1, 8>(tmp.GemmKPerBlock))
                        break;
                    if(!PreviousTwoPower<1, 4>(tmp.GemmKPack))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmNPerWave))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmMPerWave))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmNPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmMPerBlock))
                        break;

                    all_visited = true;
                } while(false);

                if(is_valid_func(tmp))
                    break;
            } while(!all_visited);
        }
        else if(problem.IsFp16())
        {
            tmp              = {256, 256, 8, 128, 128, 8, false, true, 1};
            bool all_visited = false;
            do
            {
                do
                {
                    // list in reverse order of importance,
                    // and favor large GEMM
                    if(!PreviousTwoPower<1, 8>(tmp.GemmBThreadDataPerRead_GemmN))
                        break;
                    if(!PreviousTwoPower<1, 8>(tmp.GemmKPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 8>(tmp.GemmKPack))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmNPerWave))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmMPerWave))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmNPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmMPerBlock))
                        break;

                    all_visited = true;
                } while(false);

                if(is_valid_func(tmp))
                    break;
            } while(!all_visited);
        }
        else if(problem.IsBfp16())
        {
            tmp = {256, 256, 8, 128, 128, 8, false, true, 1};

            bool all_visited = false;
            do
            {
                do
                {
                    // list in reverse order of importance,
                    // and favor large GEMM
                    if(!PreviousTwoPower<1, 8>(tmp.GemmBThreadDataPerRead_GemmN))
                        break;
                    if(!PreviousTwoPower<1, 8>(tmp.GemmKPerBlock))
                        break;
                    if(!PreviousTwoPower<2, 8>(tmp.GemmKPack))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmNPerWave))
                        break;
                    if(!PreviousTwoPower<4, 128>(tmp.GemmMPerWave))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmNPerBlock))
                        break;
                    if(!PreviousTwoPower<4, 256>(tmp.GemmMPerBlock))
                        break;

                    all_visited = true;
                } while(false);

                if(is_valid_func(tmp))
                    break;
            } while(!all_visited);
        }
        else
        {
            MIOPEN_LOG_E("Only fp32, fp16, and bfp16 are supported");
            assert(false);
        }
    };

    // first round: really valid and fast
    get_euristic_config([&ctx, &problem](const auto& config) {
        return config.IsReallyValid(problem) && config.IsFastToBeUsedForTuning(ctx, problem);
    });

    // second round: really valid
    if(!tmp.IsReallyValid(problem))
    {
        get_euristic_config([&ctx, &problem](const auto& config) {
            std::ignore = ctx;
            return config.IsReallyValid(problem);
        });
    }

    // final check
    if(!tmp.IsReallyValid(problem))
    {
        MIOPEN_LOG_I("All attempts unsuccessful");
    }
    *this = tmp;
    MIOPEN_LOG_I(ToString());
}

std::tuple<int, bool> PerformanceImplicitGemmForwardV4R5Xdlops::CalculateBlockSize() const
{
    int block_size = 0;

    try
    {
        if(!(GemmMPerBlock % GemmMPerWave == 0 && GemmNPerBlock % GemmNPerWave == 0))
            MIOPEN_THROW("invalid performance parameter");

        const auto WaveSize = 64;
        block_size = (GemmNPerBlock * GemmMPerBlock) / (GemmMPerWave * GemmNPerWave) * WaveSize;
    }
    catch(...)
    {
        return std::make_tuple(-1, false);
    }

    return std::make_tuple(block_size, true);
}

std::tuple<int, bool>
PerformanceImplicitGemmForwardV4R5Xdlops::CalculateGridSize(const ProblemDescription& problem) const
{
    int GridSize = 0;

    try
    {
        int gemm_g = -1;
        int gemm_m = -1;
        int gemm_n = -1;

        std::tie(gemm_g, gemm_m, gemm_n, std::ignore) = CalculateGemmSize(problem);

        if(!(gemm_m % GemmMPerBlock == 0 && gemm_n % GemmNPerBlock == 0))
            MIOPEN_THROW("invalid performance parameter");

        GridSize = gemm_g * (gemm_m / GemmMPerBlock) * (gemm_n / GemmNPerBlock);
    }
    catch(...)
    {
        return std::make_tuple(-1, false);
    }

    return std::make_tuple(GridSize, true);
}

std::tuple<int, int, int, int, int, bool>
PerformanceImplicitGemmForwardV4R5Xdlops::CalculateGemmABlockCopyPerformanceParameters(
    const ProblemDescription& problem) const
{
    // A tensor shape [GemmG, GemmK, GemmM, GemmKPack]

    int ClusterLengths_GemmK      = -1;
    int ClusterLengths_GemmM      = -1;
    int ClusterLengths_GemmKPack  = -1;
    int SrcDataPerRead_GemmKPack  = problem.IsFp32()
                                        ? amd_buffer_load_max_length<float>()
                                        : amd_buffer_load_max_length<half_float::half>();
    int DstDataPerWrite_GemmKPack = problem.IsFp32() ? amd_lds_write_max_length<float>()
                                                     : amd_lds_write_max_length<half_float::half>();

    try
    {
        bool valid = false;

        int block_size = -1;

        std::tie(block_size, valid) = CalculateBlockSize();

        if(!valid)
            MIOPEN_THROW("invalid performance parameter");

        // GemmKPack is src vector read dimension, bounded by GemmKPack
        SrcDataPerRead_GemmKPack = gcd(SrcDataPerRead_GemmKPack, GemmKPack);

        // calculate threadwise copy size
        auto data_per_thread_copy =
            std::max(1, (GemmKPerBlock * GemmMPerBlock * GemmKPack) / block_size);

        // make sure a thread can do a full vector load, at the cost that some threads
        // may not do threadwise copy at all
        // data_per_thread_copy = lcm(data_per_thread_copy, SrcDataPerRead_GemmKPack);

        const auto data_per_thread_copy_gemmkpack = SrcDataPerRead_GemmKPack;
        const auto tmp = data_per_thread_copy / data_per_thread_copy_gemmkpack;

        if(tmp == 0)
            MIOPEN_THROW("invalid performance parameter");

        int data_per_thread_copy_gemmk = -1;
        int data_per_thread_copy_gemmm = -1;

        if(GemmAThreadCopyMoreGemmK)
        {
            data_per_thread_copy_gemmk = gcd(GemmKPerBlock, tmp);
            data_per_thread_copy_gemmm = tmp / data_per_thread_copy_gemmk;
        }
        else
        {
            data_per_thread_copy_gemmm = gcd(GemmMPerBlock, tmp);
            data_per_thread_copy_gemmk = tmp / data_per_thread_copy_gemmm;
        }

        // vector write into LDS
        DstDataPerWrite_GemmKPack = gcd(DstDataPerWrite_GemmKPack, data_per_thread_copy_gemmkpack);

        if(!(GemmKPerBlock % data_per_thread_copy_gemmk == 0 &&
             GemmMPerBlock % data_per_thread_copy_gemmm == 0 &&
             GemmKPack % data_per_thread_copy_gemmkpack == 0))
            MIOPEN_THROW("invalid performance parameter");

        ClusterLengths_GemmK     = GemmKPerBlock / data_per_thread_copy_gemmk;
        ClusterLengths_GemmM     = GemmMPerBlock / data_per_thread_copy_gemmm;
        ClusterLengths_GemmKPack = GemmKPack / data_per_thread_copy_gemmkpack;

        // blockwise-copy support that block_size is larger than thread cluster size, which means
        // some threads may not do threadwise copy
        if(block_size != ClusterLengths_GemmK * ClusterLengths_GemmM * ClusterLengths_GemmKPack)
            MIOPEN_THROW("invalid performance parameter");
    }
    catch(...)
    {
        return std::make_tuple(-1, -1, -1, -1, -1, false);
    }

    return std::make_tuple(ClusterLengths_GemmK,
                           ClusterLengths_GemmM,
                           ClusterLengths_GemmKPack,
                           SrcDataPerRead_GemmKPack,
                           DstDataPerWrite_GemmKPack,
                           true);
}

std::tuple<int, int, int, int, int, bool>
PerformanceImplicitGemmForwardV4R5Xdlops::CalculateGemmBBlockCopyPerformanceParameters(
    const ProblemDescription& problem) const
{
    // B tensor shape [GemmG, GemmK, GemmN, GemmKPack]

    int ClusterLengths_GemmK      = -1;
    int ClusterLengths_B          = -1;
    int ClusterLengths_GemmKPack  = -1;
    int SrcDataPerRead_B          = problem.IsFp32() ? amd_buffer_load_max_length<float>()
                                                     : amd_buffer_load_max_length<half_float::half>();
    int DstDataPerWrite_GemmKPack = problem.IsFp32() ? amd_lds_write_max_length<float>()
                                                     : amd_lds_write_max_length<half_float::half>();

    try
    {
        bool valid = false;

        int block_size = -1;

        std::tie(block_size, valid) = CalculateBlockSize();

        if(!valid)
            MIOPEN_THROW("invalid performance parameter");

        // GemmN is src vector read dimension
        // calculate vector length on gemmn dimension based on global tensor layout
        const auto y               = ProblemInterpreter::GetFilterHeightY(problem);
        const auto x               = ProblemInterpreter::GetFilterWidthX(problem);
        const auto ho              = ProblemInterpreter::GetOutputHeightHo(problem);
        const auto wo              = ProblemInterpreter::GetOutputWidthWo(problem);
        const auto conv_stride_h   = ProblemInterpreter::GetAdjustedConvolutionStrideH(problem);
        const auto conv_stride_w   = ProblemInterpreter::GetAdjustedConvolutionStrideW(problem);
        const auto conv_dilation_w = ProblemInterpreter::GetAdjustedConvolutionDilationW(problem);
        const auto in_left_pad_h   = ProblemInterpreter::GetInputLeftPadH(problem);
        const auto in_left_pad_w   = ProblemInterpreter::GetInputLeftPadW(problem);
        const auto in_right_pad_h  = ProblemInterpreter::GetAdjustedInputRightPadH(problem);
        const auto in_right_pad_w  = ProblemInterpreter::GetAdjustedInputRightPadW(problem);

        const auto NWaves    = GemmNPerBlock / GemmNPerWave;
        const auto BPerBlock = GemmNPerBlock / NWaves;

        // GemmN is src vector read dimension, bounded by input tensor global memory layout
        // TODO this logic need to be more aggresive
        if(y == 1 && x == 1 && conv_stride_h == 1 && conv_stride_w == 1 && in_left_pad_h == 0 &&
           in_left_pad_w == 0 && in_right_pad_h == 0 && in_right_pad_w == 0)
        {
            SrcDataPerRead_B = gcd(SrcDataPerRead_B, ho * wo);
        }
        else if(conv_stride_w == 1 && in_left_pad_w == 0 && in_right_pad_w == 0)
        {
            SrcDataPerRead_B = gcd(SrcDataPerRead_B, wo);
        }
        else if(conv_stride_w == 1)
        {
            SrcDataPerRead_B =
                gcd(SrcDataPerRead_B, wo, in_left_pad_w, in_right_pad_w, conv_dilation_w);
        }
        else
        {
            SrcDataPerRead_B = 1;
        }

        // SrcDataPerRead_B also bounded by BPerBlock
        SrcDataPerRead_B = gcd(SrcDataPerRead_B, BPerBlock);

        // calculate threadwise copy size
        auto data_per_thread_copy =
            std::max(1, (GemmKPerBlock * NWaves * BPerBlock * GemmKPack) / block_size);
        if(miopen::IsEnabled(
               MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_FWD_V4R5_XDLOPS_ADD_VECTOR_LOAD_GEMMN_TUNE_PARAM))
        {
            if(problem.IsFp16())
            {
                if(SrcDataPerRead_B >= GemmBThreadDataPerRead_GemmN)
                {
                    SrcDataPerRead_B = GemmBThreadDataPerRead_GemmN;
                }
                else
                {
                    MIOPEN_THROW("invalid performance parameter");
                }
            }
            else
            {
                if(SrcDataPerRead_B != GemmBThreadDataPerRead_GemmN)
                {
                    MIOPEN_THROW("invalid performance parameter");
                }
            }
        }
        // make sure a thread can do a full vector load, at the cost that some threads
        // may not do threadwise copy at all
        data_per_thread_copy = lcm(data_per_thread_copy, SrcDataPerRead_B);

        const auto data_per_thread_copy_b = SrcDataPerRead_B;

        if(data_per_thread_copy_b * NWaves == 0 ||
           data_per_thread_copy % (data_per_thread_copy_b * NWaves) != 0)
            MIOPEN_THROW("invalid performance parameter");

        const auto tmp = data_per_thread_copy / (data_per_thread_copy_b * NWaves);

        int data_per_thread_copy_gemmkpack = -1;
        int data_per_thread_copy_gemmk     = -1;
        if(GemmBThreadCopyMoreGemmKPack)
        {
            data_per_thread_copy_gemmkpack = gcd(GemmKPack, tmp);
            data_per_thread_copy_gemmk     = tmp / data_per_thread_copy_gemmkpack;
        }
        else
        {
            data_per_thread_copy_gemmk     = gcd(GemmKPerBlock, tmp);
            data_per_thread_copy_gemmkpack = tmp / data_per_thread_copy_gemmk;
        }

        // vector write into LDS
        DstDataPerWrite_GemmKPack = gcd(DstDataPerWrite_GemmKPack, data_per_thread_copy_gemmkpack);

        if(!(data_per_thread_copy_gemmkpack > 0 && data_per_thread_copy_gemmk > 0 &&
             data_per_thread_copy_b > 0))
        {
            MIOPEN_THROW("invalid performance parameter");
        }

        if(!(GemmKPerBlock % data_per_thread_copy_gemmk == 0 &&
             BPerBlock % data_per_thread_copy_b == 0 &&
             GemmKPack % data_per_thread_copy_gemmkpack == 0))
            MIOPEN_THROW("invalid performance parameter");

        ClusterLengths_GemmK     = GemmKPerBlock / data_per_thread_copy_gemmk;
        ClusterLengths_B         = BPerBlock / data_per_thread_copy_b;
        ClusterLengths_GemmKPack = GemmKPack / data_per_thread_copy_gemmkpack;

        // blockwise-copy support that block_size is larger than thread cluster size, which means
        // some threads may not do threadwise copy
        if(block_size != ClusterLengths_GemmK * ClusterLengths_B * ClusterLengths_GemmKPack)
            MIOPEN_THROW("invalid performance parameter");
    }
    catch(...)
    {
        return std::make_tuple(-1, -1, -1, -1, -1, false);
    }

    return std::make_tuple(ClusterLengths_GemmK,
                           ClusterLengths_B,
                           ClusterLengths_GemmKPack,
                           SrcDataPerRead_B,
                           DstDataPerWrite_GemmKPack,
                           true);
}

std::tuple<std::size_t, bool> PerformanceImplicitGemmForwardV4R5Xdlops::CalculateLdsNumberOfByte(
    const ProblemDescription& problem) const
{
    const auto a_block_space = GemmKPerBlock * GemmMPerBlock * GemmKPack;
    const auto b_block_space = GemmKPerBlock * GemmNPerBlock * GemmKPack;

    std::size_t lds_size = (a_block_space + b_block_space) *
                           (problem.IsFp32() ? sizeof(float) : sizeof(half_float::half));

    return std::make_tuple(lds_size, true);
}

// Used by IsReallyValid()
bool PerformanceImplicitGemmForwardV4R5Xdlops::IsValidValue() const
{
    // clang-format off
    return IsTwoPower<4, 256>(GemmMPerBlock)
        && IsTwoPower<4, 256>(GemmNPerBlock)
        && IsTwoPower<1, 8>(GemmKPerBlock)
        && IsTwoPower<4, 128>(GemmMPerWave)
        && IsTwoPower<4, 128>(GemmNPerWave)
        && IsTwoPower<1, 8>(GemmKPack)
        && IsTwoPower<1, 8>(GemmBThreadDataPerRead_GemmN);
    // clang-format on
}

// Used by HeuristicInit() and GenericSearch
// Only return false if a performance config will violate requirements given by kernel algorithm
bool PerformanceImplicitGemmForwardV4R5Xdlops::IsReallyValid(
    const ProblemDescription& problem) const
{
    if(!IsValidValue())
        return false;

    if(!IsValidBlockwiseGemmXdlops(problem,
                                   GemmMPerBlock,
                                   GemmNPerBlock,
                                   GemmKPerBlock,
                                   GemmMPerWave,
                                   GemmNPerWave,
                                   GemmKPack))
        return false;

    bool valid = false;

    // check tensor contraction
    {
        const std::size_t n = ProblemInterpreter::GetBatchN(problem);
        const auto NWaves   = GemmNPerBlock / GemmNPerWave;
        // check if N can be divided by NWaves, as N will be unmerged into N1
        // and NWaves for tensor contraction
        if(n % NWaves != 0)
            return false;
    }

    // check blockwise GEMM size
    {
        int gemm_m       = -1;
        int gemm_n       = -1;
        int gemm_k_total = -1;

        std::tie(std::ignore, gemm_m, gemm_n, gemm_k_total) = CalculateGemmSize(problem);

        if(gemm_k_total % GemmKPack != 0)
            return false;

        const auto gemm_k = gemm_k_total / GemmKPack;

        if(!(gemm_m % GemmMPerBlock == 0 && gemm_n % GemmNPerBlock == 0 &&
             gemm_k % GemmKPerBlock == 0))
            return false;
    }

    // check blockwise copy of A matrix
    {
        std::tie(std::ignore, std::ignore, std::ignore, std::ignore, std::ignore, valid) =
            CalculateGemmABlockCopyPerformanceParameters(problem);

        if(!valid)
            return false;
    }

    // check blockwise copy of B matrix
    {
        std::tie(std::ignore, std::ignore, std::ignore, std::ignore, std::ignore, valid) =
            CalculateGemmBBlockCopyPerformanceParameters(problem);

        if(!valid)
            return false;
    }

    // check LDS allocation
    std::size_t lds_size      = 0;
    std::tie(lds_size, valid) = CalculateLdsNumberOfByte(problem);

    return (valid and lds_size <= get_lds_max_number_of_byte());
}

// Used by GenericSearch, not used by HeuristicInit
// Return false if a performance config is known to be sub-optimal, comparing to other performance
// config inside tuning range
bool PerformanceImplicitGemmForwardV4R5Xdlops::IsFastToBeUsedForTuning(
    const ConvolutionContext& ctx, const ProblemDescription& problem) const
{
    if(use_spare_set)
        return true;

    // somehow, 128x128 wave-wise GEMM tend to spill register
    // TODO revisit this when 128x128 wave-wise GEMM become efficient
    {
        if(GemmMPerWave * GemmNPerWave > 64 * 128)
            return false;
    }

    // don't need too many blocks
    {
        int gemm_m = 0;
        int gemm_n = 0;

        std::tie(std::ignore, gemm_m, gemm_n, std::ignore) = CalculateGemmSize(problem);

        // this is grid size using current blockwise-GEMM
        const int grid_size = (gemm_m * gemm_n) / (GemmMPerBlock * GemmNPerBlock);

        // this the the biggest blockwise-GEMM you can do
        int max_blockwise_gemm_size =
            std::max(gcd(256, gemm_m) * gcd(128, gemm_n), gcd(128, gemm_m) * gcd(256, gemm_n));

        // this is the grid size using the biggest blockwise-GEMM
        auto grid_size_max_blockwise_gemm =
            (std::size_t(gemm_m) * gemm_n) / max_blockwise_gemm_size;

        const float ratio = float(grid_size) / grid_size_max_blockwise_gemm;

        const auto num_cu = ctx.GetStream().GetMaxComputeUnits();

        // heuristic to exclude performance paramater that result in very large number of blocks
        if(grid_size_max_blockwise_gemm > 5 * num_cu)
        {
            if(ratio > 2.81)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > 4 * num_cu)
        {
            if(ratio > 3.61)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > 3 * num_cu)
        {
            if(ratio > 4.41)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > 2 * num_cu)
        {
            if(ratio > 6.41)
                return false;
        }
        else if(grid_size_max_blockwise_gemm > num_cu)
        {
            if(ratio > 12.41)
                return false;
        }
    }

    // don't need too many waves per block
    {
        const int wave_per_block = (GemmMPerBlock / GemmMPerWave) * (GemmNPerBlock / GemmNPerWave);

        if(!(wave_per_block > 1 && wave_per_block <= 4))
        {
            return false;
        }
    }

    // avoid skinny blockwise GEMM whenever possible
    {
        int gemm_m = 0;
        int gemm_n = 0;

        std::tie(std::ignore, gemm_m, gemm_n, std::ignore) = CalculateGemmSize(problem);

        if(GemmMPerBlock > 2 * GemmNPerBlock)
        {
            if(gemm_n % (2 * GemmNPerBlock) == 0)
                return false;
        }

        if(GemmNPerBlock > 2 * GemmMPerBlock)
        {
            if(gemm_m % (2 * GemmMPerBlock) == 0)
                return false;
        }
    }

    // avoid skinny wavewise GEMM whenever possible
    {
        if(GemmMPerWave > 2 * GemmNPerWave)
        {
            if(GemmNPerBlock % (2 * GemmNPerWave) == 0)
                return false;
        }

        if(GemmNPerWave > 2 * GemmMPerWave)
        {
            if(GemmMPerBlock % (2 * GemmMPerWave) == 0)
                return false;
        }
    }

    // each thread should not too much data
    {
        const int block_size = (GemmMPerBlock / GemmMPerWave) * (GemmNPerBlock / GemmNPerWave) * 64;

        const int a_data_per_thread_copy = (GemmKPerBlock * GemmMPerBlock * GemmKPack) / block_size;
        const int b_data_per_thread_copy = (GemmKPerBlock * GemmNPerBlock * GemmKPack) / block_size;

        if(problem.IsFp32())
        {
            if(a_data_per_thread_copy > 16 || b_data_per_thread_copy > 16)
                return false;
        }
        else if(problem.IsFp16() || problem.IsBfp16())
        {
            if(a_data_per_thread_copy > 32 || b_data_per_thread_copy > 32)
                return false;
        }
    }

    // GemmKPerBlock*GemmKPack should not be too small, otherwise read performance of A matrix would
    // be bad
    {
        if(problem.IsFp32())
        {
            if(GemmKPack > 4)
                return false;

            if(GemmKPerBlock * GemmKPack < 8)
                return false;
        }
        else if(problem.IsFp16() || problem.IsBfp16())
        {
            if(GemmKPerBlock * GemmKPack < 16)
                return false;
        }
    }

    // DstDataPerWrite_GemmKPack should not be too small, otherwise too many ds_write instruction
    // would cause bad performance
    {
        if(miopen::IsEnabled(
               MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_FWD_V4R5_XDLOPS_ADD_VECTOR_LOAD_GEMMN_TUNE_PARAM))
        {
            if(problem.IsFp16())
            {
                int SrcDataPerRead_B          = 0;
                int DstDataPerWrite_GemmKPack = 0;
                bool valid                    = false;
                std::tie(std::ignore,
                         std::ignore,
                         std::ignore,
                         SrcDataPerRead_B,
                         DstDataPerWrite_GemmKPack,
                         valid) = CalculateGemmBBlockCopyPerformanceParameters(problem);
                if(valid)
                {
                    if((SrcDataPerRead_B > 1) &&
                       ((DstDataPerWrite_GemmKPack == 1) || (DstDataPerWrite_GemmKPack == 2)))
                    {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

// Used by GenericSearch, not used by HeuristicInit
// Return false, if you don't want to this to be included in tuning range used by generic search
// A performance config may still be valid w.r.t algorithm correctness, even when IsValid() return
// false
bool PerformanceImplicitGemmForwardV4R5Xdlops::IsValid(const ConvolutionContext& ctx,
                                                       const ProblemDescription& problem) const
{
    return IsReallyValid(problem) && IsFastToBeUsedForTuning(ctx, problem);
}

// Used by GenericSearch, not used by HeuristicInit
bool ConvHipImplicitGemmForwardV4R5Xdlops::IsValidPerformanceConfig(
    const ProblemDescription& problem, const PerformanceImplicitGemmForwardV4R5Xdlops& config) const
{
    return config.IsReallyValid(problem);
}

PerformanceImplicitGemmForwardV4R5Xdlops
ConvHipImplicitGemmForwardV4R5Xdlops::GetDefaultPerformanceConfig(
    const ConvolutionContext& ctx, const ProblemDescription& problem) const
{
    PerformanceImplicitGemmForwardV4R5Xdlops config;
    config.HeuristicInit(ctx, problem);
    MIOPEN_LOG_I(config.ToString());
    return config;
}

ConvSolution ConvHipImplicitGemmForwardV4R5Xdlops::GetSolution(
    const ConvolutionContext& ctx,
    const ProblemDescription& problem,
    const PerformanceImplicitGemmForwardV4R5Xdlops& config) const
{
    ConvSolution result;

    if(!config.IsReallyValid(problem))
    {
        MIOPEN_LOG_E("invalid performance parameter");
        assert(false);
    }

    KernelInfo construction_parameters;

    construction_parameters.kernel_file =
        "static_kernel_gridwise_convolution_forward_implicit_gemm_v4r5_xdlops_nchw_kcyx_nkhw.cpp";

    construction_parameters.kernel_name =
        "gridwise_convolution_forward_implicit_gemm_v4r5_xdlops_nchw_kcyx_nkhw";

    int grid_size  = 0;
    int block_size = 0;

    std::tie(grid_size, std::ignore)  = config.CalculateGridSize(problem);
    std::tie(block_size, std::ignore) = config.CalculateBlockSize();

    construction_parameters.l_wk.push_back(block_size);
    construction_parameters.l_wk.push_back(1);
    construction_parameters.l_wk.push_back(1);

    construction_parameters.g_wk.push_back(static_cast<std::size_t>(block_size) * grid_size);
    construction_parameters.g_wk.push_back(1);
    construction_parameters.g_wk.push_back(1);

    int GemmABlockCopyClusterLengths_GemmK      = -1;
    int GemmABlockCopyClusterLengths_GemmM      = -1;
    int GemmABlockCopyClusterLengths_GemmKPack  = -1;
    int GemmABlockCopySrcDataPerRead_GemmKPack  = -1;
    int GemmABlockCopyDstDataPerWrite_GemmKPack = -1;

    int GemmBBlockCopyClusterLengths_GemmK      = -1;
    int GemmBBlockCopyClusterLengths_B          = -1;
    int GemmBBlockCopyClusterLengths_GemmKPack  = -1;
    int GemmBBlockCopySrcDataPerRead_GemmN      = -1;
    int GemmBBlockCopyDstDataPerWrite_GemmKPack = -1;

    std::tie(GemmABlockCopyClusterLengths_GemmK,
             GemmABlockCopyClusterLengths_GemmM,
             GemmABlockCopyClusterLengths_GemmKPack,
             GemmABlockCopySrcDataPerRead_GemmKPack,
             GemmABlockCopyDstDataPerWrite_GemmKPack,
             std::ignore) = config.CalculateGemmABlockCopyPerformanceParameters(problem);

    std::tie(GemmBBlockCopyClusterLengths_GemmK,
             GemmBBlockCopyClusterLengths_B,
             GemmBBlockCopyClusterLengths_GemmKPack,
             GemmBBlockCopySrcDataPerRead_GemmN,
             GemmBBlockCopyDstDataPerWrite_GemmKPack,
             std::ignore) = config.CalculateGemmBBlockCopyPerformanceParameters(problem);

    const auto NWaves    = config.GemmNPerBlock / config.GemmNPerWave;
    const auto BPerBlock = config.GemmNPerBlock / NWaves;
    const auto BPerWave  = config.GemmNPerWave;

    // clang-format off
    construction_parameters.comp_options =
        std::string(" -DCK_PARAM_PROBLEM_G=") + std::to_string(ProblemInterpreter::GetGroupCountG(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_N=") + std::to_string(ProblemInterpreter::GetBatchN(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_K=") + std::to_string(ProblemInterpreter::GetOutputChannelK(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_C=") + std::to_string(ProblemInterpreter::GetInputChannelC(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_HI=") + std::to_string(ProblemInterpreter::GetInputHeightHi(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_WI=") + std::to_string(ProblemInterpreter::GetInputWidthWi(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_HO=") + std::to_string(ProblemInterpreter::GetOutputHeightHo(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_WO=") + std::to_string(ProblemInterpreter::GetOutputWidthWo(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_Y=") + std::to_string(ProblemInterpreter::GetFilterHeightY(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_X=") + std::to_string(ProblemInterpreter::GetFilterWidthX(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_STRIDE_H=") + std::to_string(ProblemInterpreter::GetAdjustedConvolutionStrideH(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_STRIDE_W=") + std::to_string(ProblemInterpreter::GetAdjustedConvolutionStrideW(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_DILATION_H=") + std::to_string(ProblemInterpreter::GetAdjustedConvolutionDilationH(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_CONV_DILATION_W=") + std::to_string(ProblemInterpreter::GetAdjustedConvolutionDilationW(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_LEFT_PAD_H=") + std::to_string(ProblemInterpreter::GetInputLeftPadH(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_LEFT_PAD_W=") + std::to_string(ProblemInterpreter::GetInputLeftPadW(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_RIGHT_PAD_H=") + std::to_string(ProblemInterpreter::GetAdjustedInputRightPadH(problem)) +
        std::string(" -DCK_PARAM_PROBLEM_IN_RIGHT_PAD_W=") + std::to_string(ProblemInterpreter::GetAdjustedInputRightPadW(problem)) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_M_PER_BLOCK=") + std::to_string(config.GemmMPerBlock) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_B_PER_BLOCK=") + std::to_string(BPerBlock) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_K_PER_BLOCK=") + std::to_string(config.GemmKPerBlock) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_M_PER_WAVE=") + std::to_string(config.GemmMPerWave) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_B_PER_WAVE=") + std::to_string(BPerWave) +
        std::string(" -DCK_PARAM_TUNABLE_NWAVES=") + std::to_string(NWaves) +
        std::string(" -DCK_PARAM_TUNABLE_GEMM_KPACK=") + std::to_string(config.GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_BLOCK_SIZE=") + std::to_string(block_size) +
        std::string(" -DCK_PARAM_DEPENDENT_GRID_SIZE=") + std::to_string(grid_size) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_K=") + std::to_string(GemmABlockCopyClusterLengths_GemmK) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_M=") + std::to_string(GemmABlockCopyClusterLengths_GemmM) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_KPACK=") + std::to_string(GemmABlockCopyClusterLengths_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_SRC_DATA_PER_READ_GEMM_KPACK=") + std::to_string(GemmABlockCopySrcDataPerRead_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_A_BLOCK_COPY_DST_DATA_PER_WRITE_GEMM_KPACK=") + std::to_string(GemmABlockCopyDstDataPerWrite_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_K=") + std::to_string(GemmBBlockCopyClusterLengths_GemmK) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_B=") + std::to_string(GemmBBlockCopyClusterLengths_B) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_CLUSTER_LENGTHS_GEMM_KPACK=") + std::to_string(GemmBBlockCopyClusterLengths_GemmKPack) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_SRC_DATA_PER_READ_GEMM_N=") + std::to_string(GemmBBlockCopySrcDataPerRead_GemmN) +
        std::string(" -DCK_PARAM_DEPENDENT_GEMM_B_BLOCK_COPY_DST_DATA_PER_WRITE_GEMM_KPACK=") + std::to_string(GemmBBlockCopyDstDataPerWrite_GemmKPack) +
        std::string(" -DCK_USE_AMD_XDLOPS=") + std::to_string(IsXdlopsSupport(ctx) ? 1 : 0) +
        std::string(" -DCK_USE_AMD_XDLOPS_INLINE_ASM=") + std::to_string(miopen::IsEnabled(MIOPEN_DEBUG_IMPLICIT_GEMM_XDLOPS_INLINE_ASM) ? 1 : 0) +
        std::string(" -DCK_USE_AMD_XDLOPS_EMULATE=") + (miopen::IsEnabled(MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_XDLOPS_EMULATE) ? '1' : '0') +
        get_static_ck_common_compiler_flag(ctx) +
        ctx.general_compile_options;
    // clang-format on

    result.invoker_factory = conv::MakeImplGemmDataInvokerFactory(problem);
    result.construction_params.push_back(construction_parameters);
    return result;
}

bool ConvHipImplicitGemmForwardV4R5Xdlops::IsApplicable(const ConvolutionContext& ctx,
                                                        const ProblemDescription& problem) const
{
    if(miopen::IsDisabled(MIOPEN_DEBUG_CONV_IMPLICIT_GEMM_HIP_FWD_V4R5_XDLOPS))
        return false;

    if(problem.conv_problem.GetConv().attribute.deterministic)
        return false;

    if(!ctx.use_hip_kernels)
        return false;

    if(!IsComposableKernelSupportedHardware(ctx))
        return false;

    if(!IsXdlopsSupport(ctx))
        return false;

    if(!(problem.IsFp32() || problem.IsFp16() || problem.IsBfp16()))
        return false;

    const auto y = ProblemInterpreter::GetFilterHeightY(problem);
    const auto x = ProblemInterpreter::GetFilterWidthX(problem);

    // disable the solver for conv1x1 due to perf regression
    if(y == 1 && x == 1)
        return false;

    if(!problem.direction.IsForward())
        return false;

    if(!problem.Is2d())
        return false;

    if(ctx.GetStream().GetDeviceName() == "gfx90a" &&
       problem.conv_problem.IsGfx90aFp16altRequired())
        return false;

    if(!IsIndexRangeLargeEnough(problem))
        return false;

    if(!problem.IsLayoutDefault())
    {
        return false;
    }
    // gemm size
    {
        int gemm_g       = -1;
        int gemm_m       = -1;
        int gemm_n       = -1;
        int gemm_k_total = -1;

        std::tie(gemm_g, gemm_m, gemm_n, gemm_k_total) = CalculateGemmSize(problem);

        if(!IsValidGridGemmXdlops(gemm_m, gemm_n, gemm_k_total))
            return false;
    }

    // this particular HeuristicInit is so comprehensive, that if it cannot predict a valid
    // performance config, the problem is probably not applicable
    PerformanceImplicitGemmForwardV4R5Xdlops config;
    config.HeuristicInit(ctx, problem);

    return config.IsReallyValid(problem);
}

PerformanceImplicitGemmForwardV4R5Xdlops
ConvHipImplicitGemmForwardV4R5Xdlops::Search(const ConvolutionContext& ctx,
                                             const ProblemDescription& problem,
                                             const AnyInvokeParams& invoke_ctx) const
{
    return GenericSearch(*this, ctx, problem, invoke_ctx);
}

} // namespace solver
} // namespace miopen
