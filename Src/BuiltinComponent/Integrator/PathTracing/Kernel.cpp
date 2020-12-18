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
        const auto* data = static_cast<const Data*>(SBTData);

        Spectrum<Dimensionless<float>> pf{ { 1.0f }, { 1.0f }, { 1.0f } };
        sample = { { 0.0f }, { 0.0f }, { 0.0f } };
        for(uint32_t i = 0; i < data->maxDepth; ++i) {
            TraceResult res;
            piperTrace(context, ray, 0.0f, 1e5f, res);
            if(res.kind == TraceKind::Missing) {
                Spectrum<Radiance> rad;
                piperMissing(context, ray, rad);
                sample += pf * rad;
                return;
            }
            const auto& surface = res.surface;

            auto localDir = surface.transform(ray.direction);
            auto wo = surface.intersect.local2Shading(-localDir);

            SurfaceStorage storage;
            const auto Ng = surface.intersect.local2Shading(surface.intersect.Ng);
            piperSurfaceInit(context, surface.instance, ray.t, surface.intersect.texCoord, Ng, storage);

            auto hit = ray.origin + ray.direction * Distance{ surface.t };
            LightSample light;
            piperLightSample(context, hit, ray.t, light);
            // TODO:MIS
            if(light.pdf.val > 0.0f) {
                // TODO:occlude test
                auto delta = light.src - hit;
                auto wi = surface.intersect.local2Shading(surface.transform(Normal<float, FOR::World>{ delta }));
                Spectrum<Dimensionless<float>> f;
                piperSurfaceEvaluate(context, surface.instance, storage, wo, wi, Ng, BxDFPart::All, f);
                sample += pf * f * light.rad / light.pdf;
            }
            SurfaceSample ss;
            piperSurfaceSample(context, surface.instance, storage, wo, Ng, BxDFPart::All, ss);
            if(ss.pdf.val <= 0.0f)
                return;
            pf = pf * ss.f / ss.pdf;
            ray.direction = surface.transform(surface.intersect.shading2Local(ss.wi));
            ray.origin = hit +
                surface.transform(surface.intersect.Ng) *
                    Distance{ ss.part & BxDFPart::Reflection ? 0.001f : -0.001f };  // TODO:better bias
        }
    }
    static_assert(std::is_same_v<IntegratorFunc, decltype(&trace)>);
}  // namespace Piper
