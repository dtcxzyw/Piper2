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
    extern "C" void rayGen(RestrictedContext*, const void* SBTData, const Vector2<float>& NDC, const float u1, const float u2,
                           RayInfo<FOR::World>& ray, Dimensionless<float>& weight) {
        const auto* data = static_cast<const PCData*>(SBTData);
        const auto filmHit =
            data->anchor + data->offX * Dimensionless<float>{ 1.0f - NDC.x } + data->offY * Dimensionless<float>{ 1.0f - NDC.y };
        const auto lensOffset = sampleUniformDisk(u1, u2);
        const auto lensHit = data->lensCenter + data->apertureX * lensOffset.x + data->apertureY * lensOffset.y;
        const auto dir = data->lensCenter - filmHit;
        const auto planeOfFocusHit = data->lensCenter + dir * (data->focalDistance / dot(dir, data->forward));
        ray.origin = lensHit;
        ray.direction = normalize(planeOfFocusHit - ray.origin);
        ray.t = 0.0f;  // TODO:motion blur
        weight = { 1.0f };
    }
    static_assert(std::is_same_v<SensorFunc, decltype(&rayGen)>);
}  // namespace Piper
