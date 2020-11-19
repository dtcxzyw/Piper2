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

#pragma once
#include "PhysicalQuantitySI.hpp"
#include "Transform.hpp"
#include <cstdint>

namespace Piper {
    // TODO:alignment
    using Distance = Length<float>;
    // TODO:medium info
    struct RayInfo final {
        Point<Distance, FOR::World> origin;
        Normal<Distance, FOR::World> direction;
        float minT, maxT;
    };

    enum class Face { Front, Back };

    struct BuiltinHitInfo final {
        Normal<Distance, FOR::Local> Ng;
        Vector2<float> barycentric;
        uint32_t index;
        Face face;
    };

    struct CustomHitInfo final {
        unsigned char data[sizeof(BuiltinHitInfo)];
    };

    struct HitInfo final {
        union {
            BuiltinHitInfo builtin;
            CustomHitInfo custom;
        };
    };

    struct SurfaceIntersectionInfo final {
        Normal<Distance, FOR::Local> T, B, N;
        Vector2<float> texCoord;
        Normal<Distance, FOR::Shading> local2Shading(Normal<Distance, FOR::Local> v) const noexcept {
            return { Vector<Distance, FOR::Shading>{ dot(T, v), dot(B, v), dot(N, v) }, Unchecked{} };
        }
        Normal<Distance, FOR::Local> shading2Local(Normal<Distance, FOR::Shading> v) const noexcept {
            return { Vector<Distance, FOR::Local>{ dot(T.x, v.x) + dot(B.x, v.y) + dot(N.x, v.z),
                                                   dot(T.y, v.x) + dot(B.y, v.y) + dot(N.y, v.z),
                                                   dot(T.z, v.x) + dot(B.z, v.y) + dot(N.z, v.z) },
                     Unchecked{} };
        }
    };

    struct FullContext;
    struct RestrictedContext;

    inline RestrictedContext* decay(FullContext* context) {
        return reinterpret_cast<RestrictedContext*>(context);
    }

    // TODO:Sampled Spectrum
    template <typename Float>
    struct Spectrum final {
        Float r, g, b;
        Spectrum operator+(Spectrum rhs) const noexcept {
            return { r + rhs.r, g + rhs.g, b + rhs.b };
        }
        template <typename U>
        auto operator*(U scalar) const noexcept {
            using V = decltype(r * scalar);
            return Spectrum<V>(r * scalar, g * scalar, b * scalar);
        }
        template <typename U>
        auto operator*(Spectrum<U> rhs) const noexcept {
            using V = decltype(r * rhs.r);
            return Spectrum<V>(r * rhs.r, g * rhs.g, b * rhs.b);
        }
    };

    using SensorFunc = void (*)(RestrictedContext* context, const void* SBTData, uint32_t x, uint32_t y, RayInfo& ray,
                                Vector2<float>& point);
    using EnvironmentFunc = void (*)(RestrictedContext* context, const void* SBTData, const RayInfo& ray,
                                     Spectrum<LuminousFlux<float>>& radiance);
    enum class RayType { Reflection, Refraction };
    struct SurfaceSample final {
        Normal<Distance, FOR::Shading> wo;
        Spectrum<Dimensionless<float>> f;
        RayType type;
    };
    using SurfaceSampleFunc = void (*)(RestrictedContext* context, const void* SBTData, const Normal<Distance, FOR::Shading>& wi,
                                       SurfaceSample& sample);
    using SurfaceEvaluateFunc = void (*)(RestrictedContext* context, const void* SBTData,
                                         const Normal<Distance, FOR::Shading>& wi, const Normal<Distance, FOR::Shading>& wo,
                                         Spectrum<Dimensionless<float>>& f);
    using GeometryFunc = void (*)(RestrictedContext* context, const void* SBTData, const HitInfo& hit,
                                  SurfaceIntersectionInfo& info);
    using RenderDriverFunc = void (*)(RestrictedContext* context, const void* SBTData, const Vector2<float>& point,
                                      const Spectrum<LuminousFlux<float>>& sample);
    using IntegratorFunc = void (*)(FullContext* context, const void* SBTData, RayInfo& ray,
                                    Spectrum<LuminousFlux<float>>& sample);
    struct LightSample final {
        Point<Distance, FOR::World> src;
        Spectrum<LuminousFlux<float>> rad;
        bool valid;
    };
    using LightFunc = void (*)(RestrictedContext* context, const void* SBTData, const Point<Distance, FOR::World>& hit,
                               LightSample& sample);

    enum class TraceKind { Surface, Missing };
    struct TraceSurface final {
        SurfaceIntersectionInfo intersect;
        Transform<Distance, Dimensionless<float>, FOR::World, FOR::Local> transform;
        uint64_t instance;
        float t;
    };
    struct TraceResult final {
        TraceKind kind;
        union {
            TraceSurface surface;
        };
    };

    extern "C" {
    void piperMissing(FullContext* context, const RayInfo& ray, Spectrum<LuminousFlux<float>>& radiance);
    void piperSurfaceSample(FullContext* context, uint64_t instance, const Normal<Distance, FOR::Shading>& wi,
                            SurfaceSample& sample);
    void piperSurfaceEvaluate(FullContext* context, uint64_t instance, const Normal<Distance, FOR::Shading>& wi,
                              const Normal<Distance, FOR::Shading>& wo, Spectrum<Dimensionless<float>>& f);
    void piperLightSample(FullContext* context, const Point<Distance, FOR::World>& hit, LightSample& sample);

    void piperTrace(FullContext* context, const RayInfo& ray, float minT, float maxT, TraceResult& result);
    // TODO:terminate/ignore insection
    float piperSample(RestrictedContext* context);
    }
}  // namespace Piper
