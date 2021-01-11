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
    extern "C" void rayGen(RestrictedContext context, const void* SBTData, const Vector2<float>& NDC, const float u1,
                           const float u2, RayInfo<FOR::World>& ray, Dimensionless<float>& weight) {
        const auto* data = static_cast<const PCData*>(SBTData);

        Transform<Distance, FOR::Local, FOR::World> transform;
        piperQueryTransform(context, data->traversal, transform);
        // TODO:consider rotate/scale?
        const auto base = transform.originRefB();
        const auto forward = Normal<float, FOR::World>{ data->lookAt - base };
        const auto right = cross(forward, data->upRef);
        const auto up = cross(right, forward);
        const auto anchor = base + up * Distance{ data->size.x * 0.5f } - right * Distance{ data->size.y * 0.5f };  // left-top
        // TODO:AF/MF mode support
        const auto focalDistance = dot(data->lookAt - base, forward);
        const auto filmDistance = inverse(inverse(data->focalLength) - inverse(focalDistance));
        const auto lensCenter = base + forward * filmDistance;
        const auto offX = right * Distance{ data->size.x };
        const auto offY = up * Distance{ -data->size.y };
        const auto apertureX = right * data->apertureRadius;
        const auto apertureY = up * data->apertureRadius;

        const auto filmHit = anchor + offX * Dimensionless<float>{ 1.0f - NDC.x } + offY * Dimensionless<float>{ 1.0f - NDC.y };
        const auto lensOffset = sampleUniformDisk(u1, u2);
        const auto lensHit = lensCenter + apertureX * lensOffset.x + apertureY * lensOffset.y;
        const auto dir = lensCenter - filmHit;
        const auto planeOfFocusHit = lensCenter + dir * (focalDistance / dot(dir, forward));
        ray.origin = lensHit;
        ray.direction = normalize(planeOfFocusHit - ray.origin);
        weight = { 1.0f };
    }
    static_assert(std::is_same_v<SensorFunc, decltype(&rayGen)>);
}  // namespace Piper
