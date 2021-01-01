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

#include <algorithm>

namespace Piper {
    // TODO:motion blur
    extern "C" void PIPER_CC pointInit(RestrictedContext*, const void*, float, void*) {}
    static_assert(std::is_same_v<LightInitFunc, decltype(&pointInit)>);
    extern "C" void PIPER_CC pointSample(RestrictedContext*, const void* SBTData, const void*,
                                         const Point<Distance, FOR::World>& hit, LightSample& sample) {
        const auto* data = static_cast<const PointLightData*>(SBTData);
        const auto delta = data->pos - hit;
        const auto dis2 = lengthSquared(delta);
        sample.rad = data->intensity / dis2;
        sample.dir = Normal<float, FOR::World>{ delta / sqrtSafe(dis2), Unsafe{} };
        sample.pdf = Dimensionless<float>{ 1.0f };
    }
    static_assert(std::is_same_v<LightSampleFunc, decltype(&pointSample)>);
    extern "C" void PIPER_CC deltaEvaluate(RestrictedContext*, const void*, const void*, const Point<Distance, FOR::World>&,
                                           const Normal<float, FOR::World>&, Spectrum<Radiance>& rad) {
        rad = {};
    }
    static_assert(std::is_same_v<LightEvaluateFunc, decltype(&deltaEvaluate)>);
    extern "C" void PIPER_CC deltaPdf(RestrictedContext*, const void*, const void*, const Point<Distance, FOR::World>&,
                                      const Normal<float, FOR::World>&, Dimensionless<float>& pdf) {
        pdf = Dimensionless<float>{ 0.0f };
    }
    static_assert(std::is_same_v<LightPdfFunc, decltype(&deltaPdf)>);
    extern "C" void PIPER_CC SEInit(RestrictedContext*, const void*, float, void*) {}
    static_assert(std::is_same_v<LightInitFunc, decltype(&SEInit)>);
    extern "C" void PIPER_CC SESample(RestrictedContext* context, const void* SBTData, const void*,
                                      const Point<Distance, FOR::World>& hit, LightSample& sample) {
        const auto* data = static_cast<const SampledEnvironmentData*>(SBTData);
        sample.rad = data->texture;
        const Dimensionless<float> u = { piperSample(context) }, v = { piperSample(context) };
        const auto theta = u * Radian<float>{ Constants::pi<float> }, phi = v * Radian<float>{ Constants::pi<float> };
        const auto cosTheta = cos(theta), sinTheta = sin(theta), cosPhi = cos(phi), sinPhi = sin(phi);
        // TODO:transform
        sample.dir = Normal<float, FOR::World>{ { sinTheta * cosPhi, sinTheta * sinPhi, cosTheta }, Unsafe{} };
        sample.pdf = { sinTheta.val == 0.0f ?
                           Dimensionless<float>{ 0.0f } :
                           Dimensionless<float>{ 1.0f } / (Dimensionless<float>{ 2.0f * Constants::sqrPi<float> } * sinTheta) };
    }
    static_assert(std::is_same_v<LightSampleFunc, decltype(&SESample)>);
    extern "C" void PIPER_CC SEEvaluate(RestrictedContext*, const void* SBTData, const void*, const Point<Distance, FOR::World>&,
                                        const Normal<float, FOR::World>&, Spectrum<Radiance>& rad) {
        rad = static_cast<const SampledEnvironmentData*>(SBTData)->texture;
    }
    static_assert(std::is_same_v<LightEvaluateFunc, decltype(&SEEvaluate)>);
    extern "C" void PIPER_CC SEPdf(RestrictedContext*, const void*, const void*, const Point<Distance, FOR::World>&,
                                   const Normal<float, FOR::World>& dir, Dimensionless<float>& pdf) {
        // TODO:transform
        const auto theta = acosSafe(dir.z);  //, phi = atan2(dir.y, dir.x);
        const auto sinTheta = sin(theta);
        pdf = { sinTheta.val == 0.0f ?
                    Dimensionless<float>{ 0.0f } :
                    Dimensionless<float>{ 1.0f } / (Dimensionless<float>{ 2.0f * Constants::sqrPi<float> } * sinTheta) };
    }
    static_assert(std::is_same_v<LightPdfFunc, decltype(&SEPdf)>);
    extern "C" void PIPER_CC uniformSelect(RestrictedContext* context, const void* SBTData, LightSelectResult& result) {
        const auto count = *static_cast<const uint32_t*>(SBTData);
        const auto u = piperSample(context);
        result.light = std::min(static_cast<uint32_t>(std::floor(u * static_cast<float>(count))), count - 1);
        // TODO:remap u?
        result.delta = *(static_cast<const bool*>(SBTData) + sizeof(uint32_t) + result.light);
        result.pdf = { 1.0f / static_cast<float>(count) };
    }
    static_assert(std::is_same_v<LightSelectFunc, decltype(&uniformSelect)>);
}  // namespace Piper
