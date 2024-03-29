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

#include "Shared.hpp"

namespace Piper {
    extern "C" {
    void piperGetTaskIndex(const TaskContext context, Dim3& index) {
        index = reinterpret_cast<const TaskContextImpl*>(context)->taskIndex;
    }
    void piperGetRootResourceLUT(const TaskContext context, ResourceHandle& handle) {
        handle = reinterpret_cast<const TaskContextImpl*>(context)->LUT;
    }
    void piperLookUpResourceHandle(TaskContext, const ResourceHandle LUT, const uint32_t index, ResourceHandle& handle) {
        handle = reinterpret_cast<const ResourceHandle*>(LUT)[index];
    }
    }
}  // namespace Piper
