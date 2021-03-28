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
#define BUILTIN_CONSTANT(NAME) \
    extern uint32_t NAME##X(); \
    extern uint32_t NAME##Y(); \
    extern uint32_t NAME##Z();

    BUILTIN_CONSTANT(gridDim)
    BUILTIN_CONSTANT(blockDim)
    BUILTIN_CONSTANT(blockIdx)
    BUILTIN_CONSTANT(threadIdx)

#undef BUILTIN_CONSTANT

    void piperGetTaskIndex(const TaskContext, Dim3& index) {
        index.x = blockIdxX();
        index.y = blockIdxY();
        index.z = blockIdxZ() * blockDimX() + threadIdxX();
    }

    struct UInt32Pair final {
        uint32_t first;
        uint32_t second;
    };

    void piperGetArgument(const TaskContext context, const uint32_t index, void* ptr) {
        const auto offset1 = *reinterpret_cast<const uint32_t*>(context);
        const auto offset2 = *(reinterpret_cast<const uint32_t*>(context) + 1);
        const auto desc = reinterpret_cast<const UInt32Pair*>(reinterpret_cast<const std::byte*>(context) + offset1);
        const auto beg = reinterpret_cast<const std::byte*>(context) + (offset2 + desc[index].first);
        memcpy(ptr, beg, desc[index].second);
    }
    void piperGetRootResourceLUT(const TaskContext, ResourceHandle& handle) {
        handle = 0;
    }
    void piperLookUpResourceHandle(const TaskContext context, const ResourceHandle LUT, const uint32_t index,
                                   ResourceHandle& handle) {
        const auto base =
            reinterpret_cast<const ResourceHandle*>(reinterpret_cast<const std::byte*>(context) + 2 * sizeof(uint32_t));
        handle = base[LUT + index];
    }
    }

}  // namespace Piper
