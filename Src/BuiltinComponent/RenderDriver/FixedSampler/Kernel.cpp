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

#include "Shared.hpp"

namespace Piper {
    void atomicAdd(RGBW& x, const Spectrum<Radiance>& y, const Dimensionless<float> weight) {
        piperFloatAtomicAdd(x.radiance.r.val, y.r.val);
        piperFloatAtomicAdd(x.radiance.g.val, y.g.val);
        piperFloatAtomicAdd(x.radiance.b.val, y.b.val);
        piperFloatAtomicAdd(x.weight.val, weight.val);
    }

    extern "C" {
    void accumulate(RestrictedContext context, const void* SBTData, const void* launchData, const Vector2<float>& point,
                    const Spectrum<Radiance>& sample) {
        CallInfo filter;
        piperQueryCall(context, static_cast<const RDData*>(SBTData)->filter, filter);

        const auto* data = static_cast<const LaunchData*>(launchData);
        // TODO:filter
        const auto add = [&](const uint32_t px, const uint32_t py) {
            if(!(px < data->w && py < data->h))
                return;
            Dimensionless<float> weight;
            reinterpret_cast<FilterFunc>(filter.address)(context, filter.SBTData, point.x - (static_cast<float>(px) + 0.5f),
                                                         point.y - (static_cast<float>(py) + 0.5f), weight);
            // TODO:optimize access
            atomicAdd(data->rgbw[px + py * data->w], sample, weight);
        };
        const auto bx = static_cast<int32_t>(point.x - 0.5f), by = static_cast<int32_t>(point.y - 0.5f);
        add(bx, by);
        add(bx + 1, by);
        add(bx, by + 1);
        add(bx + 1, by + 1);
    }
    static_assert(std::is_same_v<RenderDriverFunc, decltype(&accumulate)>);

    void box(RestrictedContext, const void*, float, float, Dimensionless<float>& w) {
        w = { 1.0f };
    }
    static_assert(std::is_same_v<FilterFunc, decltype(&box)>);

    void triangle(RestrictedContext, const void*, const float dx, const float dy, Dimensionless<float>& w) {
        w = { std::fmax(0.0f, 1.0f - std::fabs(dx)) * std::fmax(0.0f, 1.0f - std::fabs(dy)) };
    }
    static_assert(std::is_same_v<FilterFunc, decltype(&triangle)>);

    void gaussian(RestrictedContext, const void* SBTData, const float dx, const float dy, Dimensionless<float>& w) {
        const auto* data = static_cast<const GaussianFilterData*>(SBTData);
        const auto weight = [&](const float d) { return std::fmax(std::exp(data->negAlpha * d * d) - data->sub, 0.0f); };
        w = { weight(dx) * weight(dy) };
    }
    static_assert(std::is_same_v<FilterFunc, decltype(&gaussian)>);
    }
}  // namespace Piper
