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
    static Spectrum<Radiance> multipleImportanceSampling(FullContext* context, const Point<Distance, FOR::World>& hit,
                                                         const float t, uint64_t instance, const SurfaceStorage& storage,
                                                         const Normal<float, FOR::Shading>& wo, const TraceSurface& surface,
                                                         const Normal<float, FOR::Shading>& Ng, bool deltaLight) {
        constexpr auto require =
            static_cast<BxDFPart>(static_cast<uint32_t>(BxDFPart::All) ^ static_cast<uint32_t>(BxDFPart::Specular));

        auto heuristic = [](Dimensionless<float> lightPdf, Dimensionless<float> BSDFPdf) {
            const auto sp1 = lightPdf * lightPdf;
            const auto sp2 = BSDFPdf * BSDFPdf;
            return sp1 / (sp1 + sp2);
        };

        auto sampleLightSource = [&] {
            LightSample light;
            piperLightSample(context, hit, t, light);
            if(light.pdf.val > 0.0f && light.rad.valid()) {
                const auto wi = surface.intersect.local2Shading(surface.transform(light.dir));
                Spectrum<Dimensionless<float>> f;
                piperSurfaceEvaluate(context, surface.instance, storage, wo, wi, Ng, require, f);
                if(f.valid()) {
                    auto occlude = false;
                    const RayInfo shadowRay{ hit, light.dir, t };
                    piperOcclude(context, shadowRay, 1e-3f, 1e5f, occlude);
                    if(!occlude) {
                        auto w = abs(wi.z) / light.pdf;
                        if(!deltaLight) {
                            Dimensionless<float> pdf;
                            piperSurfacePdf(context, instance, storage, wo, wi, Ng, require, pdf);
                            w = w * heuristic(light.pdf, pdf);
                        }
                        return f * light.rad * w;
                    }
                }
            }
            return Spectrum<Radiance>{};
        };
        /*
        auto sampleBSDF = [&] {
            if(deltaLight)
                return Spectrum<Radiance>{};
            SurfaceSample sample;
            piperSurfaceSample(context, instance, storage, wo, Ng, require, sample);

            const auto dir = surface.transform(surface.intersect.shading2Local(sample.wi));
            auto weight = abs(sample.wi.z) / sample.pdf;
            if(sample.pdf.val > 0.0f && sample.f.valid()) {
                Dimensionless<float> pdf;
                piperLightPdf(context, hit, dir, pdf);
                if(pdf.val <= 0.0f)
                    return Spectrum<Radiance>{};
                weight = heuristic(sample.pdf, pdf);
            }

            TraceResult traceResult;
            piperTrace(context, RayInfo{ hit, dir, t }, 0.001f, 1e5f, traceResult);
            auto lightHit = hit;
            if(traceResult.kind == TraceKind::Surface) {
                // hit selected area light
                lightHit = hit + dir * traceResult.surface.t;
            } else {
                // hit environment
            }

            Spectrum<Radiance> rad;
            piperLightEvaluate(context, lightHit, dir, rad);
            return rad * sample.f * weight;
        };
        */
        return sampleLightSource();  // + sampleBSDF();
    }
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

            auto hit = ray.origin + ray.direction * surface.t;

            // TODO:environment/area light
            sample += multipleImportanceSampling(context, hit, ray.t, surface.instance, storage, wo, surface, Ng, true);

            SurfaceSample ss;
            piperSurfaceSample(context, surface.instance, storage, wo, Ng, BxDFPart::All, ss);
            if(ss.pdf.val <= 0.0f)
                return;
            pf = pf * ss.f / ss.pdf;

            // Russian roulette
            // TODO:better p estimation
            if(i > 3) {
                auto p = std::fmax(0.05f, 1.0f - pf.luminosity().val);
                if(piperSample(decay(context)) < p)
                    return;
                pf = pf / Dimensionless<float>{ 1.0f - p };
            }

            ray.direction = surface.transform(surface.intersect.shading2Local(ss.wi));
            ray.origin = hit +
                surface.transform(surface.intersect.Ng) *
                    Distance{ ss.part & BxDFPart::Reflection ? 0.001f : -0.001f };  // TODO:better bias
        }
    }
    static_assert(std::is_same_v<IntegratorFunc, decltype(&trace)>);
}  // namespace Piper
