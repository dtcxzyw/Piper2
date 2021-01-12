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
    extern "C" {
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
    }
}  // namespace Piper
