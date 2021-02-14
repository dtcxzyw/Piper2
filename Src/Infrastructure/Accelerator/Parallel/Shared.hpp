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
#include "../../../Kernel/DeviceRuntime.hpp"

namespace Piper {
    struct TaskContextImpl final {
        Dim3 gridSize;
        Dim3 gridIndex;
        Dim3 blockSize;
        uint32_t gridLinearIndex;

        uint32_t index;
        uint32_t blockLinearIndex;
        Dim3 blockIndex;
    };
}  // namespace Piper
