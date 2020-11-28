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

#include "../../../Kernel/Sampling.hpp"
#include "Shared.hpp"

namespace Piper {
    extern "C" void PIPER_CC rayGen(RestrictedContext* context, const void* SBTData, const uint32_t x, const uint32_t y,
                                    const uint32_t w, const uint32_t h, const SensorNDCAffineTransform& transform, RayInfo& ray,
                                    Vector2<float>& point) {
        point.x = static_cast<float>(x) + piperSample(context);
        point.y = static_cast<float>(y) + piperSample(context);
        const auto* data = static_cast<const PCData*>(SBTData);
        const auto filmHit = data->anchor +
            data->offX * Dimensionless<float>{ 1.0f - (transform.ox + transform.sx * point.x / static_cast<float>(w)) } +
            data->offY * Dimensionless<float>{ 1.0f - (transform.oy + transform.sy * point.y / static_cast<float>(h)) };
        const auto lensOffset = sampleUniformDisk(Vector2<Dimensionless<float>>{ { piperSample(context) }, { piperSample(context) } });
        const auto lensHit = data->lensCenter + data->apertureX * lensOffset.x + data->apertureY * lensOffset.y;
        const auto dir = data->lensCenter - filmHit;
        const auto planeOfFocusHit = data->lensCenter + dir * (data->focalDistance / dot(dir, data->forward));
        ray.origin = lensHit;
        ray.direction = normalize(planeOfFocusHit - ray.origin);
        ray.t = 0.0f;  // TODO:motion blur
    }
    static_assert(std::is_same_v<SensorFunc, decltype(&rayGen)>);
}  // namespace Piper
