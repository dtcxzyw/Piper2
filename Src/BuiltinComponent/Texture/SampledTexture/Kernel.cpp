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
#include <algorithm>

namespace Piper {
    template <typename T>
    T repeat(const T val, const T mod) {
        const auto rem = std::remainder(val, mod);
        return rem < 0.0f ? rem + mod : rem;
    }

    template <typename T>
    T mirror(const T val, const T mod) {
        const auto rem = std::remainder(val, mod);
        return std::fabs(rem);
    }

    uint32_t clamp(const uint32_t x, const uint32_t max) noexcept {
        return std::max(0U, std::min(x, max));
    }

    uint32_t locate(const float p, const uint32_t size, const TextureWrap wrap) noexcept {
        const auto s = (wrap == TextureWrap::Repeat ? repeat(p, static_cast<float>(size)) : mirror(p, static_cast<float>(size)));
        return clamp(static_cast<uint32_t>(s), size - 1);
    }

    uint32_t next(const uint32_t p, const uint32_t size, const TextureWrap wrap) noexcept {
        const auto s = p + 1;
        return s == size ? (wrap == TextureWrap::Repeat ? 0 : p) : s;
    }

    void evaluate(const uint32_t u, const uint32_t v, const float weight, const Data* data, const uint8_t* texel,
                  Dimensionless<float>* sample) {
        const auto idx = v * data->stride + u * data->channel;
        for(uint32_t i = 0; i < data->channel; ++i)
            sample[i].val += weight * static_cast<float>(texel[idx + i]);
    }

    float evalLeftWeight(const float u) {
        return 0.5f + std::floor(u + 0.5f) - u;
    }

    extern "C" void sampleTexture(const RestrictedContext context, const void* SBTData, const Vector2<float>& texCoord,
                                  Dimensionless<float>* sample) {
        const auto* data = static_cast<const Data*>(SBTData);
        TimeProfiler profiler{ context, data->profileSample };
        ResourceHandle texelHandle;
        piperGetResourceHandleIndirect(context, data->texel, texelHandle);
        auto texel = reinterpret_cast<const uint8_t*>(texelHandle);

        const auto u = texCoord.x * static_cast<float>(data->width), v = texCoord.y * static_cast<float>(data->height);
        const auto plu = locate(u - 0.5f, data->width, data->wrap), pru = next(plu, data->width, data->wrap),
                   plv = locate(v - 0.5f, data->height, data->wrap), prv = next(plv, data->height, data->wrap);
        const auto lwx = evalLeftWeight(u), lwy = evalLeftWeight(v);
        const auto rwx = 1.0f - lwx, rwy = 1.0f - lwy;
        for(uint32_t i = 0; i < data->channel; ++i)
            sample[i].val = 0.0f;
        evaluate(plu, plv, lwx * lwy, data, texel, sample);
        evaluate(pru, plv, rwx * lwy, data, texel, sample);
        evaluate(plu, prv, lwx * rwy, data, texel, sample);
        evaluate(pru, prv, rwx * rwy, data, texel, sample);
        constexpr auto scale = 0.003921568f;  // 1/255
        for(uint32_t i = 0; i < data->channel; ++i)
            sample[i].val *= scale;
    }
    static_assert(std::is_same_v<TextureSampleFunc, decltype(&sampleTexture)>);
}  // namespace Piper
