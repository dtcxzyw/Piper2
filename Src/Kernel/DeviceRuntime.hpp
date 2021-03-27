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

#pragma once
#include <cstdint>

namespace Piper {
    struct Dim3 final {
        uint32_t x, y, z;
    };

    using ResourceHandle = ptrdiff_t;
    struct TaskContextReserved;
    using TaskContext = const TaskContextReserved*;

    using KernelProtocol = void (*)(TaskContext ctx);

    extern "C" {
    void piperGetGridSize(TaskContext context, Dim3& dim);
    void piperGetBlockSize(TaskContext context, Dim3& dim);
    void piperGetGridIndex(TaskContext context, Dim3& index);
    void piperGetBlockIndex(TaskContext context, Dim3& index);
    void piperGetGridLinearIndex(TaskContext context, uint32_t& index);
    void piperGetBlockLinearIndex(TaskContext context, uint32_t& index);
    void piperGetTaskIndex(TaskContext context, uint32_t& index);

    void piperGetArgument(TaskContext context, uint32_t index, void* ptr);
    void piperGetRootResourceLUT(TaskContext context, ResourceHandle& handle);
    void piperLookUpResourceHandle(TaskContext context, ResourceHandle LUT, uint32_t index, ResourceHandle& handle);

    extern int8_t* piperBuiltinSymbolLUT[];

    // TODO: Atomic Intrinsic
    // TODO: Synchronize Primitive?
    // TODO: Exception Handling
    // TODO: Profiling API from Protocol.hpp
    }

    template <typename T>
    T piperLookUpSymbol(const uint32_t index) {
        return reinterpret_cast<T>(piperBuiltinSymbolLUT[index]);
    }
}  // namespace Piper
