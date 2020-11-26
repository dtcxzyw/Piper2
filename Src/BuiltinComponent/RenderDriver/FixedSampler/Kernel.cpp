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
    extern "C" void PIPER_CC accumulate(RestrictedContext*, const void* SBTData, const Vector2<float>& point,
                                        const Spectrum<Radiance>& sample) {
        auto data = reinterpret_cast<const Data*>(SBTData);
        auto px = static_cast<uint32_t>(point.x);
        px = (px < data->w ? px : data->w - 1);
        auto py = static_cast<uint32_t>(point.y);
        py = (py < data->h ? py : data->h - 1);
        // TODO:support atomicAdd?
        // TODO:Filter
        data->res[px + py * data->w] += sample;
    }
    static_assert(std::is_same_v<RenderDriverFunc, decltype(&accumulate)>);
}  // namespace Piper
