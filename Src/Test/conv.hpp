/*
   Copyright [2020] [ZHENG Yingwei]

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
#include "../Kernel/FloatingPoint.hpp"
#include <cstdint>

PIPER_FP_TYPE(Float);

void conv(const uint32_t idx, const Float* X, const Float* Y, Float* Z, const uint32_t width, const uint32_t height,
          const uint32_t kernelSize) {
    int32_t half = kernelSize / 2;
    int32_t y = idx / width, x = idx % width;
    Float res = constantFloat(0.0);
    for(int32_t i = 0; i < static_cast<int32_t>(kernelSize); ++i) {
        int32_t nx = x + i - half;
        if(nx >= 0 && nx < static_cast<int32_t>(width))
            for(int32_t j = 0; j < static_cast<int32_t>(kernelSize); ++j) {
                int32_t ny = y + j - half;
                if(ny >= 0 && ny < static_cast<int32_t>(height)) {
                    res = fmaFloat(X[ny * width + nx], Y[i * kernelSize + j], res);
                }
            }
    }
    Z[idx] = res;
}
