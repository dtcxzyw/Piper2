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

// Based on pbrt source currently
#include "Shared.hpp"
#include "SobolMatrix.dat"

namespace Piper {
    float sobolSampleImpl(uint64_t index, const uint32_t dim, const uint32_t scramble) {
        auto v = scramble;
        for(auto i = dim * sobolMatrixSize; index; index >>= 1, i++)
            if(index & 1)
                v ^= sobolMatrices32[i];
        return static_cast<float>(v) * 0x1p-32f;
    }

    inline uint64_t sobolIntervalToIndex(const uint32_t m, uint32_t sample, const uint32_t px, const uint32_t py) {
        if(m == 0)
            return 0;

        const auto m2 = m << 1;
        auto index = static_cast<uint64_t>(sample) << m2;

        uint64_t delta = 0;
        for(auto c = 0; sample; sample >>= 1, ++c)
            if(sample & 1)
                delta ^= vdcSobolMatrices[m - 1][c];

        auto b = ((static_cast<uint64_t>(px) << m) | py) ^ delta;

        for(auto c = 0; b; b >>= 1, ++c)
            if(b & 1)
                index ^= vdcSobolMatricesInv[m - 1][c];

        return index;
    }

    extern "C" void sobolStart(const void* SBTData, const uint32_t x, const uint32_t y, const uint32_t sample, uint64_t& idx,
                               Vector2<float>& pos) {
        const auto* data = static_cast<const SobolData*>(SBTData);
        idx = sobolIntervalToIndex(data->log2Resolution, sample, x, y);
        pos.x = static_cast<float>(x) + sobolSampleImpl(idx, 0, data->scramble);
        pos.y = static_cast<float>(y) + sobolSampleImpl(idx, 1, data->scramble);
    }
    static_assert(std::is_same_v<SampleStartFunc, decltype(&sobolStart)>);

    extern "C" void sobolGenerate(const void* SBTData, const uint64_t idx, const uint32_t dim, float& val) {
        const auto* data = static_cast<const SobolData*>(SBTData);
        val = sobolSampleImpl(idx, dim, data->scramble);
    }
    static_assert(std::is_same_v<SampleGenerateFunc, decltype(&sobolGenerate)>);
}  // namespace Piper
