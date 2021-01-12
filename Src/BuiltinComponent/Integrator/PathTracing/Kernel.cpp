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
    static Spectrum<Radiance> multipleImportanceSampling(FullContext context, const Point<Distance, FOR::World>& hit,
                                                         const SurfaceStorage& surfaceStorage,
                                                         const Normal<float, FOR::Shading>& wo, const TraceSurface& surface,
                                                         const Normal<float, FOR::Shading>& Ng) {
        auto heuristic = [](const Dimensionless<float> lightPdf, const Dimensionless<float> BSDFPdf) {
            const auto sp1 = lightPdf * lightPdf;
            const auto sp2 = BSDFPdf * BSDFPdf;
            return sp1 / (sp1 + sp2);
        };

        LightSelectResult select;
        piperLightSelect(context, select);
        if(select.pdf.val <= 0.0f)
            return Spectrum<Radiance>{};
        LightStorage lightStorage;
        piperLightInit(context, select.light, lightStorage);

        auto sampleLightSource = [&] {
            LightSample light;
            piperLightSample(context, select.light, lightStorage, hit, light);
            if(light.pdf.val > 0.0f && light.rad.valid()) {
                const auto wi = surface.intersect.local2Shading(surface.transform(light.dir));
                Spectrum<Dimensionless<float>> f;
                piperSurfaceEvaluate(context, surface.surface, surfaceStorage, wo, wi, Ng, noSpecular, f);
                if(f.valid()) {
                    const RayInfo<FOR::World> shadowRay{ hit, light.dir };
                    // TODO:better bias
                    if(!piperOcclude(context, shadowRay, std::fmin(1e-3f, light.distance.val), light.distance.val)) {
                        auto w = abs(wi.z) / light.pdf;
                        if(!select.delta) {
                            Dimensionless<float> pdf;
                            piperSurfacePdf(context, surface.surface, surfaceStorage, wo, wi, Ng, noSpecular, pdf);
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
            piperSurfaceSample(context, surface.surface, surfaceStorage, wo, Ng, noSpecular, sample);

            if(sample.pdf.val <= 0.0f || !sample.f.valid())
                return Spectrum<Radiance>{};

            auto weight = abs(sample.wi.z) / sample.pdf;
            const auto dir = surface.transform(surface.intersect.shading2Local(sample.wi));

            TraceResult traceResult;
            piperTrace(context, RayInfo<FOR::World>{ hit, dir }, 1e-3f, 1e5f, traceResult);
            Spectrum<Radiance> rad;
            if(!((traceResult.kind == TraceKind::AreaLight && select.light == traceResult.surface.light) ||
                 traceResult.kind == TraceKind::Missing))
                return Spectrum<Radiance>{};
            const auto* light = select.light;
            if(traceResult.kind == TraceKind::Missing) {
                light = environment;
                piperLightInit(context, environment, lightStorage);
            }

            // TODO:reduce useless computation for environment light
            const auto src = hit + dir * traceResult.surface.t;
            const auto normal = traceResult.surface.transform(traceResult.surface.intersect.N);
            piperLightEvaluate(context, light, lightStorage, src, normal, dir, rad);
            if(!rad.valid())
                return Spectrum<Radiance>{};

            Dimensionless<float> pdf;
            piperLightPdf(context, light, lightStorage, src, normal, dir, traceResult.surface.t, pdf);
            if(pdf.val <= 0.0f)
                return Spectrum<Radiance>{};

            weight = weight * heuristic(sample.pdf, pdf);

            return rad * sample.f * weight;
        };
        // NOTICE:Don't exchange the order of sampling!!!
        const auto partA = sampleLightSource();
        const auto partB = sampleBSDF();
        return (partA + partB) / select.pdf;
    }
    extern "C" void trace(FullContext context, const void* SBTData, RayInfo<FOR::World>& ray, Spectrum<Radiance>& sample) {
        const auto* data = static_cast<const Data*>(SBTData);
        TimeProfiler profiler{ decay(context), data->profilePathTime };
        Spectrum<Dimensionless<float>> pf{ { 1.0f }, { 1.0f }, { 1.0f } };
        sample = {};
        auto specular = true;
        uint32_t depth = 0;
        while(true) {
            TraceResult res;
            piperTrace(context, ray, 1e-3f, 1e5f, res);

            if(specular && (res.kind == TraceKind::AreaLight || res.kind == TraceKind::Missing)) {
                LightStorage storage;
                Spectrum<Radiance> rad;
                if(res.kind == TraceKind::AreaLight) {
                    // area light
                    piperLightInit(context, res.surface.light, storage);
                    // TODO:reduce computation of hit and normal
                    const auto delta = ray.direction * res.surface.t;
                    piperLightEvaluate(context, res.surface.light, storage, ray.origin + delta,
                                       res.surface.transform(res.surface.intersect.N), Normal<float, FOR::World>{ delta }, rad);
                } else if(res.kind == TraceKind::Missing) {
                    // environment
                    piperLightInit(context, environment, storage);
                    piperLightEvaluate(
                        context, environment, storage,
                        *static_cast<const Point<Distance, FOR::World>*>(nullptr),  // NOLINT(clang-diagnostic-null-dereference)
                        *static_cast<const Normal<float, FOR::World>*>(nullptr),    // NOLINT(clang-diagnostic-null-dereference)
                        ray.direction, rad);
                }

                sample += pf * rad;
            }

            if(res.kind == TraceKind::AreaLight) {
                --depth;
                ray.origin = ray.origin + ray.direction * (res.surface.t + Distance{ 0.001f });
                continue;
            }

            if(res.kind == TraceKind::Missing || depth >= data->maxDepth)
                break;
            const auto& surface = res.surface;

            const auto localDir = surface.transform(ray.direction);
            const auto wo = surface.intersect.local2Shading(-localDir);

            SurfaceStorage storage;
            const auto Ng = surface.intersect.local2Shading(surface.intersect.Ng);
            bool earlyCheck;
            piperSurfaceInit(context, surface.surface, surface.intersect.texCoord, Ng, res.surface.intersect.face,
                             TransportMode::Radiance, storage, earlyCheck);

            const auto hit = ray.origin + ray.direction * surface.t;
            if(earlyCheck)
                sample += pf * multipleImportanceSampling(context, hit, storage, wo, surface, Ng);
            SurfaceSample ss;
            piperSurfaceSample(context, surface.surface, storage, wo, Ng, BxDFPart::All, ss);
            const auto valid = ss.pdf.val > 0.0f && ss.f.valid();
            piperStatisticsBool(decay(context), data->profileValidRay, valid);
            if(!valid)
                break;

            pf = pf * ss.f * (abs(ss.wi.z) / ss.pdf);

            // TODO:BSSRDF

            // Russian roulette
            // TODO:better p estimation(etaScale)
            if(depth > 3) {
                const auto p = std::fmax(0.05f, 1.0f - std::fmax(pf.r.val, std::fmax(pf.g.val, pf.b.val)));
                if(piperSample(context) < p)
                    break;
                pf = pf / Dimensionless<float>{ 1.0f - p };
            }

            ray.direction = surface.transform(surface.intersect.shading2Local(ss.wi));
            ray.origin = hit +
                surface.transform(surface.intersect.Ng) *
                    Distance{ ss.part & BxDFPart::Reflection ? 0.001f : -0.001f };  // TODO:better bias
            specular = ss.part & BxDFPart::Specular;

            ++depth;
        }
        piperStatisticsUInt(decay(context), data->profileDepth, depth + 1);
    }
    static_assert(std::is_same_v<IntegratorFunc, decltype(&trace)>);
}  // namespace Piper
