/*
 * Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
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
 */

#ifndef TRT_PLUGIN_KERNELS_H
#define TRT_PLUGIN_KERNELS_H

#include "NvInfer.h"
#include <cassert>
#include <cub/cub.cuh>
#include <pluginUtil.h>
#include <vector>

namespace bert
{

template <typename T, unsigned TPB>
__global__ void scaledSoftmaxKernelSmall(const int ld, const float rsqrtHeadSize, const T* input, T* output)
{
    scaledSoftmaxSmall<T, TPB>(ld, ld, rsqrtHeadSize, input, output);
}

template <typename T, unsigned TPB>
__global__ void scaledSoftmaxKernel(const int ld, const float rsqrtHeadSize, const T* input, T* output)
{
    scaledSoftmax<T, TPB>(ld, ld, rsqrtHeadSize, input, output);
}

template <typename T>
int computeScaledSoftmax(
    cudaStream_t stream, const int ld, const int B, const int N, const float rsqrtHeadSize, const T* input, T* output)
{

    const dim3 grid(ld * N, B, 1);

    if (ld <= 32)
    {
        const int blockSize = 32;
        scaledSoftmaxKernelSmall<T, blockSize><<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, input, output);
    }
    else if (ld <= 128)
    {
        const int blockSize = 128;
        scaledSoftmaxKernelSmall<T, blockSize><<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, input, output);
    }
    else if (ld == 384)
    {
        const int blockSize = 384;
        scaledSoftmaxKernelSmall<T, blockSize><<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, input, output);
    }
    else
    {
        const int blockSize = 256;

        scaledSoftmaxKernel<T, blockSize><<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, input, output);
    }

    CHECK(cudaPeekAtLastError());
    return 0;
}
#if 0
template <typename T, unsigned TPB>
__global__ void maskedScaledSoftmaxKernelSmall(
    const int ld, const float rsqrtHeadSize, const int* maskIdx, const T* input, T* output)
{
    __shared__ int lastValid;

    if (threadIdx.x == 0)
    {
        //sds-temp, ld是当前句子的长度，maskIdx中保存了每个batchid的mask 分界点
        //所以，这个kernel的粒度，要求每个block正好对应一个sentence
        //另外，这个写法只能是左对齐
        lastValid = min(ld, maskIdx[blockIdx.y]);
    }
    __syncthreads();

    scaledSoftmaxSmall<T, TPB>(ld, lastValid, rsqrtHeadSize, input, output);
}

template <typename T, unsigned TPB>
__global__ void maskedScaledSoftmaxKernel(
    const int ld, const float rsqrtHeadSize, const int* maskIdx, const T* input, T* output)
{

    __shared__ int lastValid;

    if (threadIdx.x == 0)
    {
        lastValid = min(ld, maskIdx[blockIdx.y]);
    }
    __syncthreads();
    scaledSoftmax<T, TPB>(ld, lastValid, rsqrtHeadSize, input, output);
}
#endif

template <typename T, unsigned TPB>
__global__ void maskedScaledSoftmaxKernelSmall(
    const int ld, const float rsqrtHeadSize, const int* maskIdx, const T* input, T* output)
{
  

    using BlockReduce = cub::BlockReduce<float, TPB>;

    __shared__ typename BlockReduce::TempStorage tmpStorage;

    __shared__ float rZ;

    const int offset = (blockIdx.y * gridDim.x + blockIdx.x) * ld;

    const float w(rsqrtHeadSize);
    cub::Sum sum;
    float threadData(0);

    const int idx = offset + threadIdx.x;

	//sds-temp, right-padding
	#if 0
    if (threadIdx.x < lastValid)
    {
        const float val = input[idx];
        threadData = exp(val * w);
    }
	#endif
	//sds-temp, 改成left-padding

    //==0 is you usefule
	if (fabs(maskIdx[blockIdx.y * ld +threadIdx.x] - 0.0) <= 1e-6)
    {
        const float val = input[idx];
        threadData = exp(val * w);
    }
	  

    const auto Z = BlockReduce(tmpStorage).Reduce(threadData, sum);

    if (threadIdx.x == 0)
    {
        rZ = (1.f) / Z;
    }
    __syncthreads();

    if (threadIdx.x < ld)
    {
        // this will be 0 for threadIdx.x >= lastValid 
        // this will be 0 for threadIdx.x < lastValid, sds-temp
        output[idx] = T(threadData * rZ);
    }

}

template <typename T, unsigned TPB>
__global__ void maskedScaledSoftmaxKernel(
    const int ld, const float rsqrtHeadSize, const int* maskIdx, const T* input, T* output)
{

    using BlockReduce = cub::BlockReduce<float, TPB>;
    __shared__ typename BlockReduce::TempStorage tmpStorage;

    __shared__ float rZ;

    const int offset = (blockIdx.y * gridDim.x + blockIdx.x) * ld;

    const float w(rsqrtHeadSize);
    cub::Sum sum;
    float threadData(0);

    for (int i = threadIdx.x; i < ld; i += TPB)
    {
	    if (fabs(maskIdx[blockIdx.y * ld +i] - 0.0) <= 1e-6)
	    {
	        const int idx = offset + i;
	        const float val = input[idx];
	        threadData += exp(val * w);
	    }
    }

    const auto Z = BlockReduce(tmpStorage).Reduce(threadData, sum);

    if (threadIdx.x == 0)
    {
        rZ = 1.f / Z;
    }
    __syncthreads();

    for (int i = threadIdx.x; i < ld; i += TPB)
    {
        const int idx = offset + i;
        const float val = (fabs(maskIdx[blockIdx.y * ld +i] - 0.0) <= 1e-6) ? exp(float(input[idx]) * w) * rZ : 0.f;
        output[idx] = T(val);
    }
}



template <typename T>
int computeMaskedScaledSoftmax(cudaStream_t stream, const int ld, const int B, const int N, const float rsqrtHeadSize,
    const int* maskIdx, const T* input, T* output)
{
    // Mask idx is of length B and assumes the valid region is contiguous starting
    // from the beginning of the sequence

    const dim3 grid(ld * N, B, 1);//(128*8, 10,1)

    if (ld <= 32)
    {
        const int blockSize = 32;
        maskedScaledSoftmaxKernelSmall<T, blockSize>
            <<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, maskIdx, input, output);
    }
    else if (ld <= 128)
    {
        const int blockSize = 128;
        maskedScaledSoftmaxKernelSmall<T, blockSize>
            <<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, maskIdx, input, output);
    }
    else if (ld == 384)
    {
        const int blockSize = 384;
        maskedScaledSoftmaxKernelSmall<T, blockSize>
            <<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, maskIdx, input, output);
    }
    else
    {
        const int blockSize = 256;

        maskedScaledSoftmaxKernel<T, blockSize>
            <<<grid, blockSize, 0, stream>>>(ld, rsqrtHeadSize, maskIdx, input, output);
    }

    CHECK(cudaPeekAtLastError());
    return 0;
}

template <unsigned TPB>
__global__ void maskIdxKernelSmall(int ld, const int* mask, int* maskIdx)
{

    using BlockReduce = cub::BlockReduce<int, TPB>;
    __shared__ typename BlockReduce::TempStorage tmpStorage;

    // ld is S
    // blockIdx.x is b

    const int offset = blockIdx.x * ld; // batch strides of S

    cub::Min min;
    int threadData(ld); // if the mask admits all values

    const int idx = offset + threadIdx.x;
    if (threadIdx.x < ld)
    {
        const int val = mask[idx];
        if (val == 0) // masked position: report thread idx
        {
            threadData = threadIdx.x;
        }
    }

    const auto minIdx = BlockReduce(tmpStorage).Reduce(threadData, min);

    if (threadIdx.x == 0)
    {
        maskIdx[blockIdx.x] = minIdx;
    }
}

template <unsigned TPB>
__global__ void maskIdxKernel(int ld, const int* mask, int* maskIdx)
{

    using BlockReduce = cub::BlockReduce<int, TPB>;
    __shared__ typename BlockReduce::TempStorage tmpStorage;

    // ld is S
    // blockIdx.x is b

    const int offset = blockIdx.x * ld; // batch strides of S

    cub::Min min;
    int threadData(ld); // if the mask admits all values

    for (int i = threadIdx.x; i < ld; i += TPB)
    {
        const int idx = offset + i;
        const int val = mask[idx];
        if (val == 0) // masked position: report thread idx
        {
            threadData = min(threadData, i);
        }
    }

    const auto minIdx = BlockReduce(tmpStorage).Reduce(threadData, min);

    if (threadIdx.x == 0)
    {
        maskIdx[blockIdx.x] = minIdx;
    }
}

inline int computeMaskIdx(cudaStream_t stream, const int S, const int B, const int* mask, int* maskIdx)
{
    // Mask idx is of length B and assumes the valid region is contiguous starting
    // from the beginning of the sequence

    // Assume n = BxS
    if (S <= 32)
    {
        maskIdxKernelSmall<32><<<B, 32, 0, stream>>>(S, mask, maskIdx);
    }
    else if (S <= 128)
    {
        maskIdxKernelSmall<128><<<B, 128, 0, stream>>>(S, mask, maskIdx);
    }
    else if (S == 384)
    {
        maskIdxKernelSmall<384><<<B, 384, 0, stream>>>(S, mask, maskIdx);
    }
    else
    {
        maskIdxKernel<256><<<B, 256, 0, stream>>>(S, mask, maskIdx);
    }

    CHECK(cudaPeekAtLastError());

    return 0;
}
}
#endif // TRT_PLUGIN_KERNELS_H
