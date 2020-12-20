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
    constexpr auto noSpecular =
        static_cast<BxDFPart>(static_cast<uint32_t>(BxDFPart::All) ^ static_cast<uint32_t>(BxDFPart::Specular));
    static Spectrum<Radiance> multipleImportanceSampling(FullContext* context, const Point<Distance, FOR::World>& hit,
                                                         const float t, uint64_t instance, const SurfaceStorage& surfaceStorage,
                                                         const Normal<float, FOR::Shading>& wo, const TraceSurface& surface,
                                                         const Normal<float, FOR::Shading>& Ng) {
        auto heuristic = [](Dimensionless<float> lightPdf, Dimensionless<float> BSDFPdf) {
            const auto sp1 = lightPdf * lightPdf;
            const auto sp2 = BSDFPdf * BSDFPdf;
            return sp1 / (sp1 + sp2);
        };

        LightSelectResult select;
        piperLightSelect(context, select);
        if(select.pdf.val <= 0.0f)
            return Spectrum<Radiance>{};
        LightStorage lightStorage;
        piperLightInit(context, select.light, t, lightStorage);

        auto sampleLightSource = [&] {
            LightSample light;
            piperLightSample(context, select.light, lightStorage, hit, light);
            if(light.pdf.val > 0.0f && light.rad.valid()) {
                const auto wi = surface.intersect.local2Shading(surface.transform(light.dir));
                Spectrum<Dimensionless<float>> f;
                piperSurfaceEvaluate(context, surface.instance, surfaceStorage, wo, wi, Ng, noSpecular, f);
                if(f.valid()) {
                    auto occlude = false;
                    const RayInfo shadowRay{ hit, light.dir, t };
                    piperOcclude(context, shadowRay, 1e-3f, 1e5f, occlude);
                    if(!occlude) {
                        auto w = abs(wi.z) / light.pdf;
                        if(!select.delta) {
                            Dimensionless<float> pdf;
                            piperSurfacePdf(context, instance, surfaceStorage, wo, wi, Ng, noSpecular, pdf);
                            w = w * heuristic(light.pdf, pdf);
                        }
                        return f * light.rad * w;
                    }
                }
            }
            return Spectrum<Radiance>{};
        };
        auto sampleBSDF = [&] {
            if(select.delta)
                return Spectrum<Radiance>{};
            SurfaceSample sample;
            piperSurfaceSample(context, instance, surfaceStorage, wo, Ng, noSpecular, sample);

            const auto dir = surface.transform(surface.intersect.shading2Local(sample.wi));
            auto weight = abs(sample.wi.z) / sample.pdf;
            if(sample.pdf.val > 0.0f && sample.f.valid()) {
                Dimensionless<float> pdf;
                piperLightPdf(context, select.light, lightStorage, hit, dir, pdf);
                if(pdf.val <= 0.0f)
                    return Spectrum<Radiance>{};
                weight = heuristic(sample.pdf, pdf);
            }

            TraceResult traceResult;
            piperTrace(context, RayInfo{ hit, dir, t }, 0.001f, 1e5f, traceResult);
            Spectrum<Radiance> rad;
            if(traceResult.kind == TraceKind::Surface) {
                // hit selected area light
                // TODO:check area light
                // piperLightEvaluate(context, select.light, lightStorage, hit + dir * traceResult.surface.t, dir, rad);
                rad = {};
            } else {
                piperLightInit(context, 0, t, lightStorage);
                piperLightEvaluate(
                    context, 0, lightStorage,
                    *static_cast<const Point<Distance, FOR::World>*>(nullptr),  // NOLINT(clang-diagnostic-null-dereference)
                    dir, rad);
            }

            return rad * sample.f * weight;
        };
        const auto partA = sampleLightSource();
        const auto partB = sampleBSDF();
        return (partA + partB) / select.pdf;
    }
    extern "C" void PIPER_CC trace(FullContext* context, const void* SBTData, RayInfo& ray, Spectrum<Radiance>& sample) {
        const auto* data = static_cast<const Data*>(SBTData);

        Spectrum<Dimensionless<float>> pf{ { 1.0f }, { 1.0f }, { 1.0f } };
        sample = { { 0.0f }, { 0.0f }, { 0.0f } };
        auto specular = true;
        for(uint32_t i = 0;; ++i) {
            TraceResult res;
            piperTrace(context, ray, 0.0f, 1e5f, res);

            if(specular) {
                LightStorage storage;
                Spectrum<Radiance> rad;
                if(res.kind == TraceKind::Surface) {
                    // TODO: area light
                    rad = {};
                } else {
                    // environment
                    piperLightInit(context, 0, ray.t, storage);
                    piperLightEvaluate(
                        context, 0, storage,
                        *static_cast<const Point<Distance, FOR::World>*>(nullptr),  // NOLINT(clang-diagnostic-null-dereference)
                        ray.direction, rad);
                }
                sample += pf * rad;
            }

            if(res.kind == TraceKind::Missing || i >= data->maxDepth)
                return;
            const auto& surface = res.surface;

            auto localDir = surface.transform(ray.direction);
            auto wo = surface.intersect.local2Shading(-localDir);

            SurfaceStorage storage;
            const auto Ng = surface.intersect.local2Shading(surface.intersect.Ng);
            bool earlyCheck;
            piperSurfaceInit(context, surface.instance, ray.t, surface.intersect.texCoord, Ng, storage, earlyCheck);

            const auto hit = ray.origin + ray.direction * surface.t;

            if(earlyCheck)
                sample += multipleImportanceSampling(context, hit, ray.t, surface.instance, storage, wo, surface, Ng);
            SurfaceSample ss;
            piperSurfaceSample(context, surface.instance, storage, wo, Ng, BxDFPart::All, ss);
            if(ss.pdf.val <= 0.0f || !ss.f.valid())
                return;
            pf = pf * ss.f * (abs(ss.wi.z) / ss.pdf);

            // TODO:BSSRDF

            // Russian roulette
            // TODO:better p estimation(etaScale)
            if(i > 3) {
                const auto p = std::fmax(0.05f, 1.0f - std::fmax(pf.r.val, std::fmax(pf.g.val, pf.b.val)));
                if(piperSample(decay(context)) < p)
                    return;
                pf = pf / Dimensionless<float>{ 1.0f - p };
            }

            ray.direction = surface.transform(surface.intersect.shading2Local(ss.wi));
            ray.origin = hit +
                surface.transform(surface.intersect.Ng) *
                    Distance{ ss.part & BxDFPart::Reflection ? 0.001f : -0.001f };  // TODO:better bias
            specular = ss.part & BxDFPart::Specular;
        }
    }
    static_assert(std::is_same_v<IntegratorFunc, decltype(&trace)>);
}  // namespace Piper
