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
#include "../../../Kernel/Protocol.hpp"

namespace Piper {
    constexpr uint32_t numSobolDimensions = 1024;
    constexpr uint32_t sobolMatrixSize = 52;

    struct SobolData final {
        uint32_t resolution;
        uint32_t log2Resolution;
        uint32_t scramble;
    };
}  // namespace Piper
