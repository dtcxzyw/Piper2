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
    extern "C" void PIPER_CC rayGen(RestrictedContext* context, const void* SBTData, uint32_t x, uint32_t y, uint32_t w,
                                    uint32_t h, RayInfo& ray, Vector2<float>& point) {
        point.x = static_cast<float>(x) + piperSample(context);
        point.y = static_cast<float>(y) + piperSample(context);
        auto data = reinterpret_cast<const PCData*>(SBTData);
        ray.origin = data->anchor + data->offX * Dimensionless<float>{ 1.0f - point.x / static_cast<float>(w) } +
            data->offY * Dimensionless<float>{ 1.0f - point.y / static_cast<float>(h) };
        ray.direction = Normal<float, FOR::World>{ data->focus - ray.origin };
    }
    static_assert(std::is_same_v<SensorFunc, decltype(&rayGen)>);
}  // namespace Piper
