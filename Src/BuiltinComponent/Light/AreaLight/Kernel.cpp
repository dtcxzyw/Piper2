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

#include "../../../Kernel/Sampling.hpp"
#include "Shared.hpp"

namespace Piper {
    extern "C" void diffuseInit(const RestrictedContext, const void*, void*) {}
    static_assert(std::is_same_v<LightInitFunc, decltype(&diffuseInit)>);
    extern "C" void diffuseSample(const RestrictedContext context, const void* SBTData, const void*,
                                  const Point<Distance, FOR::World>& hit, float u1, float u2, LightSample& sample) {
        const auto* data = static_cast<const DiffuseAreaLightData*>(SBTData);
        Point<Distance, FOR::World> src;
        Normal<float, FOR::World> n;
        piperCall<GeometrySampleFunc>(context, data->sample, hit, u1, u2, src, n, sample.pdf);
        const auto delta = src - hit;
        if(sample.pdf.val == 0.0f || dot(delta, n).val >= 0.0f) {
            sample.pdf = { 0.0f };
            return;
        }
        sample.rad = data->radiance;
        const auto dis2 = lengthSquared(delta);
        sample.distance = { std::sqrt(dis2.val) };
        sample.dir = { delta / sample.distance, Unsafe{} };
        // TODO:fix unit
        sample.pdf = Dimensionless<float>{ (sample.pdf * dis2 / abs(dot(n, sample.dir))).val };
    }
    static_assert(std::is_same_v<LightSampleFunc, decltype(&diffuseSample)>);
    extern "C" void diffuseEvaluate(RestrictedContext, const void* SBTData, const void*, const Point<Distance, FOR::World>&,
                                    const Normal<float, FOR::World>& n, const Normal<float, FOR::World>& wi,
                                    Spectrum<Radiance>& rad) {
        const auto* data = static_cast<const DiffuseAreaLightData*>(SBTData);
        rad = dot(n, wi).val < 0.0f ? data->radiance : Spectrum<Radiance>{};
    }
    static_assert(std::is_same_v<LightEvaluateFunc, decltype(&diffuseEvaluate)>);
    extern "C" void diffusePdf(RestrictedContext, const void* SBTData, const void*, const Point<Distance, FOR::World>&,
                               const Normal<float, FOR::World>& n, const Normal<float, FOR::World>& dir, const Distance t,
                               Dimensionless<float>& pdf) {
        const auto* data = static_cast<const DiffuseAreaLightData*>(SBTData);
        pdf = calcGeometrySamplePdf(t, dir, n, data->area);
    }
    static_assert(std::is_same_v<LightPdfFunc, decltype(&diffusePdf)>);
}  // namespace Piper
