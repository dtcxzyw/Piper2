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

#include "conv.hpp"
#include "../PiperAPI.hpp"
#include <cstdint>

struct Payload final {
    uint64_t pX;
    uint64_t pY;
    uint64_t pZ;
    uint32_t width;
    uint32_t height;
    uint32_t kernelSize;
};

extern "C" void PIPER_CC conv(const uint32_t idx, const Payload* payload) {
    auto X = reinterpret_cast<const Float*>(payload->pX);
    auto Y = reinterpret_cast<const Float*>(payload->pY);
    auto Z = reinterpret_cast<Float*>(payload->pZ);
    conv(idx, X, Y, Z, payload->width, payload->height, payload->kernelSize);
}