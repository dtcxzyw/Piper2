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
    extern "C" void PIPER_CC lightPoint(RestrictedContext*, const void* SBTData, const Point<Distance, FOR::World>& hit, float t,
                                        LightSample& sample) {
        const auto* data = static_cast<const PointLightData*>(SBTData);
        sample.rad = data->intensity / lengthSquared(data->pos - hit);
        sample.valid = true;
        sample.src = data->pos;
    }
    static_assert(std::is_same_v<LightFunc, decltype(&lightPoint)>);
}  // namespace Piper
