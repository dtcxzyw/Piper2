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

#include "conv.hpp"
#include "../Kernel/DeviceRuntime.hpp"
#include <cstdint>
#include <type_traits>

extern "C" void convEntry(const Piper::TaskContext context) {
    using namespace Piper;
    uint32_t idx, width, height, kernelSize;
    piperGetTaskIndex(context, idx);
    piperGetArgument(context, 0, &width);
    piperGetArgument(context, 1, &height);
    piperGetArgument(context, 2, &kernelSize);

    ResourceHandle X, Y, Z;
    piperGetResourceHandle(context, 0, X);
    piperGetResourceHandle(context, 1, Y);
    piperGetResourceHandle(context, 2, Z);
    conv(idx, reinterpret_cast<const Float*>(X), reinterpret_cast<const Float*>(Y), reinterpret_cast<Float*>(Z), width, height,
         kernelSize);
}
static_assert(std::is_same_v<Piper::KernelProtocol, std::decay_t<decltype(convEntry)>>);
