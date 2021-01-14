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

using Float = float;

inline void conv(const uint32_t idx, const Float* X, const Float* Y, Float* Z, const uint32_t width, const uint32_t height,
                 const uint32_t kernelSize) {
    const auto half = kernelSize / 2;
    const auto y = idx / width;
    const auto x = idx % width;
    auto res = 0.0f;
    for(auto i = 0; i < static_cast<int32_t>(kernelSize); ++i) {
        const int32_t nx = x + i - half;
        if(nx >= 0 && nx < static_cast<int32_t>(width))
            for(auto j = 0; j < static_cast<int32_t>(kernelSize); ++j) {
                const int32_t ny = y + j - half;
                if(ny >= 0 && ny < static_cast<int32_t>(height)) {
                    res += X[ny * width + nx] * Y[i * kernelSize + j];
                }
            }
    }
    Z[idx] = res;
}
