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

#include "../../../Kernel/DeviceRuntime.hpp"
#include "Shared.hpp"

namespace Piper {
    extern "C" {
    float piperSample(const FullContext context) {
        auto* ctx = reinterpret_cast<PerSampleContext*>(context);
        if(ctx->currentDimension < ctx->argument.maxDimension) {
            float res;
            ctx->argument.generate(ctx->argument.SAPayload, ctx->sampleIndex, ctx->currentDimension++, res);
            return res;
        }
        return std::generate_canonical<float, std::numeric_limits<size_t>::max()>(ctx->randomEngine);
    }

    void piperMain(const TaskContext ctx) {
        uint64_t beg;
        piperGetTime(nullptr, beg);
        KernelArgument SBT{};
        piperGetArgument(ctx, 0, &SBT);
        uint32_t sampleIdx;
        piperGetBlockLinearIndex(ctx, sampleIdx);
        Dim3 pixelIdx;
        piperGetGridIndex(ctx, pixelIdx);
        pixelIdx.x += SBT.rect.left - SBT.fullRect.left;
        pixelIdx.y += SBT.rect.top - SBT.fullRect.top;

        PerSampleContext context{
            SBT,
            ctx,
            { 0.0f },
            5,
            0,
            RandomEngine{ static_cast<uint64_t>(pixelIdx.x * SBT.fullRect.height + pixelIdx.y) * SBT.sampleCount + sampleIdx }
        };
        Vector2<float> point;
        SBT.start(SBT.SAPayload, pixelIdx.x, pixelIdx.y, sampleIdx, context.sampleIndex, point);
        point.x += static_cast<float>(SBT.fullRect.left + pixelIdx.x);
        point.y += static_cast<float>(SBT.fullRect.top + pixelIdx.y);
        SBT.generate(SBT.SAPayload, context.sampleIndex, 2, context.time.val);

        // TODO:move to transform
        const auto& transform = SBT.transform;
        const auto NDC = Vector2<float>{ transform.ox + transform.sx * point.x / static_cast<float>(SBT.width),
                                         transform.oy + transform.sy * point.y / static_cast<float>(SBT.height) };
        RayInfo<FOR::World> ray;
        Dimensionless<float> weight;
        {
            float u1, u2;
            SBT.generate(SBT.SAPayload, context.sampleIndex, 3, u1);
            SBT.generate(SBT.SAPayload, context.sampleIndex, 4, u2);
            SBT.rayGen(reinterpret_cast<RestrictedContext>(&context), SBT.RGPayload, NDC, u1, u2, ray, weight);
        }

        Spectrum<Radiance> sample;
        SBT.trace(reinterpret_cast<FullContext>(&context), SBT.TRPayload, ray, sample);
        SBT.accumulate(reinterpret_cast<RestrictedContext>(&context), SBT.ACPayload, SBT.launchData, point, sample * weight);

        uint64_t end;
        piperGetTime(nullptr, end);
        piperStatisticsTime(reinterpret_cast<RestrictedContext>(&context), SBT.profileSampleTime, end - beg);
    }
    static_assert(std::is_same_v<KernelProtocol, std::decay_t<decltype(piperMain)>>);

    Time<float> piperQueryTime(const RestrictedContext context) {
        return reinterpret_cast<PerSampleContext*>(context)->time;
    }

    void piperSurfaceInit(const FullContext context, const SurfaceHandle surface, const Vector2<float>& texCoord,
                          const Normal<float, FOR::Shading>& Ng, const Face face, const TransportMode mode,
                          SurfaceStorage& storage, bool& noSpecular) {
        const auto* func = reinterpret_cast<const GSMInstanceUserData*>(surface);
        func->init(decay(context), func->SFPayload, texCoord, Ng, face, mode, &storage, noSpecular);
    }
    void piperSurfaceSample(const FullContext context, const SurfaceHandle surface, const SurfaceStorage& storage,
                            const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng, const BxDFPart require,
                            SurfaceSample& sample) {
        const auto* func = reinterpret_cast<const GSMInstanceUserData*>(surface);
        func->sample(decay(context), func->SFPayload, &storage, wo, Ng, require, piperSample(context), piperSample(context),
                     sample);
    }
    void piperSurfaceEvaluate(const FullContext context, const SurfaceHandle surface, const SurfaceStorage& storage,
                              const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                              const Normal<float, FOR::Shading>& Ng, const BxDFPart require, Spectrum<Dimensionless<float>>& f) {
        const auto* func = reinterpret_cast<const GSMInstanceUserData*>(surface);
        func->evaluate(decay(context), func->SFPayload, &storage, wo, wi, Ng, require, f);
    }
    void piperSurfacePdf(const FullContext context, const SurfaceHandle surface, const SurfaceStorage& storage,
                         const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                         const Normal<float, FOR::Shading>& Ng, const BxDFPart require, Dimensionless<float>& pdf) {
        const auto* func = reinterpret_cast<const GSMInstanceUserData*>(surface);
        func->pdf(decay(context), func->SFPayload, &storage, wo, wi, Ng, require, pdf);
    }
    void piperLightSelect(const FullContext context, LightSelectResult& select) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->lightSample(decay(context), ctx->LSPayload, piperSample(context), select);
        select.light = reinterpret_cast<LightHandle>(ctx->lights + reinterpret_cast<ptrdiff_t>(select.light));
    }
    void piperLightInit(const FullContext context, const LightHandle light, LightStorage& storage) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->init(decay(context), func->LIPayload, &storage);
    }
    void piperLightSample(const FullContext context, const LightHandle light, const LightStorage& storage,
                          const Point<Distance, FOR::World>& hit, LightSample& sample) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->sample(decay(context), func->LIPayload, &storage, hit, piperSample(context), piperSample(context), sample);
    }
    void piperLightEvaluate(const FullContext context, const LightHandle light, const LightStorage& storage,
                            const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& n,
                            const Normal<float, FOR::World>& dir, Spectrum<Radiance>& rad) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->evaluate(decay(context), func->LIPayload, &storage, lightSourceHit, n, dir, rad);
    }
    void piperLightPdf(const FullContext context, const LightHandle light, const LightStorage& storage,
                       const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& n,
                       const Normal<float, FOR::World>& dir, const Distance t, Dimensionless<float>& pdf) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->pdf(decay(context), func->LIPayload, &storage, lightSourceHit, n, dir, t, pdf);
    }

    void piperQueryCall(const RestrictedContext context, const CallHandle call, CallInfo& info) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        info = ctx->callInfo[reinterpret_cast<ptrdiff_t>(call)];
    }

    // TODO:per-vertex TBN
    void calcTriangleMeshSurface(RestrictedContext, const void* payload, const void* hitInfo, SurfaceIntersectionInfo& info) {
        const auto& hit = *static_cast<const BuiltinHitInfo*>(hitInfo);
        const auto* buffer = static_cast<const BuiltinTriangleBuffer*>(payload);
        info.N = info.Ng = (hit.face == Face::Front ? hit.Ng : -hit.Ng);

        // see https://www.embree.org/api.html#rtc_geometry_type_triangle
        const auto pu = buffer->index[hit.index * 3 + 1], pv = buffer->index[hit.index * 3 + 2],
                   pw = buffer->index[hit.index * 3];
        const auto u = hit.barycentric.x, v = hit.barycentric.y, w = 1.0f - u - v;

        const Normal<float, FOR::Local> u1{ { { 1.0f }, { 0.0f }, { 0.0f } }, Unsafe{} };
        const Normal<float, FOR::Local> u2{ { { 0.0f }, { 1.0f }, { 0.0f } }, Unsafe{} };
        if(fabsf(dot(info.N, u1).val) < fabsf(dot(info.N, u2).val))
            info.T = cross(info.N, u1);
        else
            info.T = cross(info.N, u2);
        info.B = cross(info.N, info.T);
        if(buffer->texCoord)
            info.texCoord = buffer->texCoord[pu] * u + buffer->texCoord[pv] * v + buffer->texCoord[pw] * w;
        else
            info.texCoord = { 0.0f, 0.0f };
        info.face = hit.face;
    }
    static_assert(std::is_same_v<GeometryPostProcessFunc, decltype(&calcTriangleMeshSurface)>);
    void piperGetResourceHandleIndirect(const RestrictedContext context, const uint32_t index, ResourceHandle& handle) {
        piperGetResourceHandle(reinterpret_cast<PerSampleContext*>(context)->ctx, index, handle);
    }
    }
}  // namespace Piper
