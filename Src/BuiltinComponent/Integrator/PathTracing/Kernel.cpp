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
    extern "C" void PIPER_CC trace(FullContext* context, const void* SBTData, RayInfo& ray, Spectrum<Radiance>& sample) {
        auto data = reinterpret_cast<const Data*>(SBTData);

        Spectrum<Dimensionless<float>> pf{ { 1.0f }, { 1.0f }, { 1.0f } };
        sample = { { 0.0f }, { 0.0f }, { 0.0f } };
        for(uint32_t i = 0; i < data->maxDepth; ++i) {
            TraceResult res;
            piperTrace(context, ray, 0.0f, 1e5f, res);
            if(res.kind == TraceKind::Missing) {
                Spectrum<Radiance> res;
                piperMissing(context, ray, res);
                sample += pf * res;
                return;
            }
            auto& surface = res.surface;

            auto localDir = surface.transform(ray.direction);
            auto wi = surface.intersect.local2Shading(-localDir);

            auto hit = ray.origin + ray.direction * Distance{ surface.t };
            LightSample light;
            piperLightSample(context, hit, light);
            if(light.valid) {
                // TODO:occlude test
                auto delta = light.src - hit;
                auto wo = surface.intersect.local2Shading(surface.transform(Normal<float, FOR::World>{ delta }));
                Spectrum<Dimensionless<float>> f;
                piperSurfaceEvaluate(context, surface.instance, wi, wo, f);
                sample += pf * f * light.rad;
            }

            SurfaceSample ss;
            piperSurfaceSample(context, surface.instance, wi, ss);
            pf = pf * ss.f;
            ray.direction = surface.transform(surface.intersect.shading2Local(ss.wo));
            ray.origin = hit;  // TODO:bias?
        }
    }
    static_assert(std::is_same_v<IntegratorFunc, decltype(&trace)>);
}  // namespace Piper
