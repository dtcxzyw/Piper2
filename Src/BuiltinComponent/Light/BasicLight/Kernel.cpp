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
    struct PointStorage final {
        Point<Distance, FOR::World> pos;
    };

    static_assert(sizeof(PointStorage) <= sizeof(LightStorage));

    extern "C" void pointInit(const RestrictedContext context, const void* SBTData, void* storage) {
        const auto* data = static_cast<const PointLightData*>(SBTData);
        Transform<Distance, FOR::Local, FOR::World> transform;
        piperQueryTransform(context, data->traversal, transform);
        auto* info = static_cast<PointStorage*>(storage);
        info->pos = transform.originRefB();
    }
    static_assert(std::is_same_v<LightInitFunc, decltype(&pointInit)>);
    extern "C" void pointSample(RestrictedContext, const void* SBTData, const void* storage,
                                const Point<Distance, FOR::World>& hit, float, float, LightSample& sample) {
        const auto* data = static_cast<const PointLightData*>(SBTData);
        const auto delta = static_cast<const PointStorage*>(storage)->pos - hit;
        const auto dis2 = lengthSquared(delta);
        sample.rad = data->intensity / dis2;
        sample.distance = sqrtSafe(dis2);
        sample.dir = Normal<float, FOR::World>{ delta / sample.distance, Unsafe{} };
        sample.pdf = Dimensionless<float>{ 1.0f };
    }
    static_assert(std::is_same_v<LightSampleFunc, decltype(&pointSample)>);
    extern "C" void deltaEvaluate(RestrictedContext, const void*, const void*, const Point<Distance, FOR::World>&,
                                  const Normal<float, FOR::World>&, const Normal<float, FOR::World>&, Spectrum<Radiance>& rad) {
        rad = {};
    }
    static_assert(std::is_same_v<LightEvaluateFunc, decltype(&deltaEvaluate)>);
    extern "C" void deltaPdf(RestrictedContext, const void*, const void*, const Point<Distance, FOR::World>&,
                             const Normal<float, FOR::World>&, const Normal<float, FOR::World>&, Distance,
                             Dimensionless<float>& pdf) {
        pdf = Dimensionless<float>{ 0.0f };
    }
    static_assert(std::is_same_v<LightPdfFunc, decltype(&deltaPdf)>);
    // TODO:transform
    extern "C" void SEInit(RestrictedContext, const void*, void*) {}
    static_assert(std::is_same_v<LightInitFunc, decltype(&SEInit)>);
    extern "C" void SESample(RestrictedContext, const void* SBTData, const void*, const Point<Distance, FOR::World>&,
                             const float u1, const float u2, LightSample& sample) {
        const auto* data = static_cast<const SampledEnvironmentData*>(SBTData);
        sample.rad = data->texture;
        const auto theta = Radian<float>{ u1 * Constants::pi<float> }, phi = Radian<float>{ u2 * Constants::pi<float> };
        const auto cosTheta = cos(theta), sinTheta = sin(theta), cosPhi = cos(phi), sinPhi = sin(phi);
        sample.dir = Normal<float, FOR::World>{ { sinTheta * cosPhi, sinTheta * sinPhi, cosTheta }, Unsafe{} };
        sample.pdf = { sinTheta.val == 0.0f ?
                           Dimensionless<float>{ 0.0f } :
                           Dimensionless<float>{ 1.0f } / (Dimensionless<float>{ 2.0f * Constants::sqrPi<float> } * sinTheta) };
        // TODO:scene bound
        sample.distance = { 1e5f };
    }
    static_assert(std::is_same_v<LightSampleFunc, decltype(&SESample)>);
    extern "C" void SEEvaluate(RestrictedContext, const void* SBTData, const void*, const Point<Distance, FOR::World>&,
                               const Normal<float, FOR::World>&, const Normal<float, FOR::World>&, Spectrum<Radiance>& rad) {
        rad = static_cast<const SampledEnvironmentData*>(SBTData)->texture;
    }
    static_assert(std::is_same_v<LightEvaluateFunc, decltype(&SEEvaluate)>);
    extern "C" void SEPdf(RestrictedContext, const void*, const void*, const Point<Distance, FOR::World>&,
                          const Normal<float, FOR::World>&, const Normal<float, FOR::World>& dir, Distance,
                          Dimensionless<float>& pdf) {
        // TODO:transform
        const auto theta = acosSafe(dir.z);  //, phi = atan2(dir.y, dir.x);
        const auto sinTheta = sin(theta);
        pdf = { sinTheta.val == 0.0f ?
                    Dimensionless<float>{ 0.0f } :
                    Dimensionless<float>{ 1.0f } / (Dimensionless<float>{ 2.0f * Constants::sqrPi<float> } * sinTheta) };
    }
    static_assert(std::is_same_v<LightPdfFunc, decltype(&SEPdf)>);
    extern "C" void uniformSelect(RestrictedContext, const void* SBTData, const float u, LightSelectResult& result) {
        const auto count = *static_cast<const uint32_t*>(SBTData);
        const auto select = std::min(static_cast<uint32_t>(std::floor(u * static_cast<float>(count))), count - 1);
        result.light = reinterpret_cast<LightHandle>(static_cast<ptrdiff_t>(select));  // TODO:better interface
        // TODO:remap u for reuse?
        result.delta = *(static_cast<const bool*>(SBTData) + sizeof(uint32_t) + select);
        result.pdf = { 1.0f / static_cast<float>(count) };
    }
    static_assert(std::is_same_v<LightSelectFunc, decltype(&uniformSelect)>);
}  // namespace Piper
