/*
   Copyright 2020-2021 Yingwei Zheng
   SPDX-License-Identifier: Apache-2.0

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "../../../Kernel/DeviceRuntime.hpp"
#include <cstddef>
#include <cstring>

namespace Piper {
    extern "C" {
    struct dim3 final {
        unsigned int x, y, z;
    };
    extern const dim3 gridDim;
    extern const dim3 blockDim;
    extern const dim3 blockIdx;
    extern const dim3 threadIdx;

    void piperGetGridSize(const TaskContext, Dim3& dim) {
        dim.x = gridDim.x;
        dim.y = gridDim.y;
        dim.z = gridDim.z;
    }
    void piperGetBlockSize(const TaskContext, Dim3& dim) {
        dim.x = blockDim.x;
        dim.y = blockDim.y;
        dim.z = blockDim.z;
    }
    void piperGetGridIndex(const TaskContext, Dim3& index) {
        index.x = blockIdx.x;
        index.y = blockIdx.y;
        index.z = blockIdx.z;
    }
    void piperGetBlockIndex(const TaskContext, Dim3& index) {
        index.x = threadIdx.x;
        index.y = threadIdx.y;
        index.z = threadIdx.z;
    }
    void piperGetGridLinearIndex(const TaskContext, uint32_t& index) {
        index = (blockIdx.x * gridDim.y + blockIdx.y) * gridDim.z + blockIdx.z;
    }
    void piperGetBlockLinearIndex(const TaskContext context, uint32_t& index) {
        index = (threadIdx.x * blockDim.y + threadIdx.y) * blockDim.z + threadIdx.z;
    }
    void piperGetTaskIndex(const TaskContext, uint32_t& index) {
        const auto idx0 = (blockIdx.x * gridDim.y + blockIdx.y) * gridDim.z + blockIdx.z;
        const auto idx1 = (threadIdx.x * blockDim.y + threadIdx.y) * blockDim.z + threadIdx.z;
        index = idx0 * (blockIdx.x * blockIdx.y * blockIdx.z) + idx1;
    }
    struct UInt32Pair final {
        uint32_t first;
        uint32_t second;
    };

    void piperGetArgument(const TaskContext context, const uint32_t index, void* ptr) {
        const auto offset = *(reinterpret_cast<const uint32_t*>(context) + 1);
        const auto base = reinterpret_cast<const std::byte*>(context) + 2 * sizeof(uint32_t);
        const auto desc = reinterpret_cast<const UInt32Pair*>(reinterpret_cast<const std::byte*>(context) + offset);
        const auto beg = base + desc[index].first;
        memcpy(ptr, beg, desc[index].second);
    }
    void piperGetRootResourceLUT(const TaskContext context, ResourceHandle& handle) {
        handle = 0;
    }
    void piperLookUpResourceHandle(const TaskContext context, const ResourceHandle LUT, const uint32_t index,
                                   ResourceHandle& handle) {
        const auto offset = *reinterpret_cast<const uint32_t*>(context);
        const auto base = reinterpret_cast<const ResourceHandle*>(reinterpret_cast<const std::byte*>(context) + offset);
        handle = base[LUT + index];
    }
    }

}  // namespace Piper
