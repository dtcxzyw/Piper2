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
#include "../PiperAPI.hpp"
#include "PhysicalQuantitySI.hpp"
#include "Transform.hpp"
#include <cstdint>

namespace Piper {
    // TODO:alignment
    using Distance = Length<float>;
    // TODO:medium info
    struct RayInfo final {
        Point<Distance, FOR::World> origin;
        Normal<float, FOR::World> direction;
    };

    enum class Face { Front, Back };

    struct BuiltinHitInfo final {
        Normal<float, FOR::Local> Ng;
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
        Normal<float, FOR::Local> T, B, N;
        Vector2<float> texCoord;
        Normal<float, FOR::Shading> local2Shading(Normal<float, FOR::Local> v) const noexcept {
            return { Vector<Dimensionless<float>, FOR::Shading>{ dot(T, v), dot(B, v), dot(N, v) }, Unchecked{} };
        }
        Normal<float, FOR::Local> shading2Local(Normal<float, FOR::Shading> v) const noexcept {
            return { Vector<Dimensionless<float>, FOR::Local>{ dot(T.x, v.x) + dot(B.x, v.y) + dot(N.x, v.z),
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
            return Spectrum<V>{ r * scalar, g * scalar, b * scalar };
        }
        template <typename U>
        auto operator/(U scalar) const noexcept {
            using V = decltype(r / scalar);
            return Spectrum<V>{ r / scalar, g / scalar, b / scalar };
        }
        template <typename U>
        auto operator*(Spectrum<U> rhs) const noexcept {
            using V = decltype(r * rhs.r);
            return Spectrum<V>{ r * rhs.r, g * rhs.g, b * rhs.b };
        }
        Spectrum& operator+=(Spectrum rhs) noexcept {
            r = r + rhs.r, g = g + rhs.g, b = b + rhs.b;
            return *this;
        }
    };
    using Flux = Power<float>;
    using Irradiance = Ratio<Flux, Area<float>>;
    using Intensity = Ratio<Flux, SolidAngle<float>>;
    using Radiance = Ratio<Irradiance, SolidAngle<float>>;

    using SensorFunc = void (*)(RestrictedContext* context, const void* SBTData, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                RayInfo& ray, Vector2<float>& point);
    using EnvironmentFunc = void (*)(RestrictedContext* context, const void* SBTData, const RayInfo& ray,
                                     Spectrum<Radiance>& radiance);
    enum class RayType { Reflection, Refraction };

    struct SurfaceSample final {
        Normal<float, FOR::Shading> wo;
        Spectrum<Dimensionless<float>> f;
        RayType type;
    };
    using SurfaceSampleFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData,
                                              const Normal<float, FOR::Shading>& wi, SurfaceSample& sample);
    using SurfaceEvaluateFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData,
                                                const Normal<float, FOR::Shading>& wi, const Normal<float, FOR::Shading>& wo,
                                                Spectrum<Dimensionless<float>>& f);
    using GeometryFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const HitInfo& hit,
                                         SurfaceIntersectionInfo& info);
    using RenderDriverFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const Vector2<float>& point,
                                             const Spectrum<Radiance>& sample);
    using IntegratorFunc = void(PIPER_CC*)(FullContext* context, const void* SBTData, RayInfo& ray, Spectrum<Radiance>& sample);
    struct LightSample final {
        Point<Distance, FOR::World> src;
        Spectrum<Radiance> rad;
        bool valid;
    };
    using LightFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const Point<Distance, FOR::World>& hit,
                                      LightSample& sample);

    enum class TraceKind { Surface, Missing };
    struct TraceSurface final {
        SurfaceIntersectionInfo intersect;
        Transform<Distance, FOR::World, FOR::Local> transform;
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
    void PIPER_CC piperMissing(FullContext* context, const RayInfo& ray, Spectrum<Radiance>& radiance);
    void PIPER_CC piperSurfaceSample(FullContext* context, uint64_t instance, const Normal<float, FOR::Shading>& wi,
                                     SurfaceSample& sample);
    void PIPER_CC piperSurfaceEvaluate(FullContext* context, uint64_t instance, const Normal<float, FOR::Shading>& wi,
                                       const Normal<float, FOR::Shading>& wo, Spectrum<Dimensionless<float>>& f);
    void PIPER_CC piperLightSample(FullContext* context, const Point<Distance, FOR::World>& hit, LightSample& sample);

    void PIPER_CC piperTrace(FullContext* context, const RayInfo& ray, float minT, float maxT, TraceResult& result);
    // TODO:terminate/ignore insection
    float PIPER_CC piperSample(RestrictedContext* context);
    // TODO:debug interface
    // void PIPER_CC piperPrint(RestrictedContext* context, const char* msg);
    }
}  // namespace Piper
