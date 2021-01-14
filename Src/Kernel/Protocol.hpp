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

#pragma once
#include "PhysicalQuantitySI.hpp"
#include "Transform.hpp"
#include <cstdint>

namespace Piper {
    // Now only 64-bit platform is supported.
    static_assert(sizeof(ptrdiff_t) == 8);

    // TODO:alignment
    using Distance = Length<float>;
    // TODO:medium info
    template <FOR ref>
    struct RayInfo final {
        Point<Distance, ref> origin;
        Normal<float, ref> direction;
    };

    enum class Face { Front, Back };

    struct BuiltinTriangleBuffer {
        const uint32_t* index;
        const Vector2<float>* texCoord;
        const Vector<float, FOR::Local>* Ns;
        const Vector<float, FOR::Local>* Ts;
    };

    struct GeometryStorage final {
        std::byte data[32];
    };

    struct SurfaceIntersectionInfo final {
        Normal<float, FOR::Local> T, B, N, Ng;
        Vector2<float> texCoord;
        Face face;

        [[nodiscard]] auto local2Shading(Normal<float, FOR::Local> v) const noexcept {
            return Normal<float, FOR::Shading>{ Vector<Dimensionless<float>, FOR::Shading>{ dot(T, v), dot(B, v), dot(N, v) },
                                                Unsafe{} };
        }

        [[nodiscard]] auto shading2Local(Normal<float, FOR::Shading> v) const noexcept {
            return Normal<float, FOR::Local>{ Vector<Dimensionless<float>, FOR::Local>{
                                                  dot(T.x, v.x) + dot(B.x, v.y) + dot(N.x, v.z),
                                                  dot(T.y, v.x) + dot(B.y, v.y) + dot(N.y, v.z),
                                                  dot(T.z, v.x) + dot(B.z, v.y) + dot(N.z, v.z) },
                                              Unsafe{} };
        }
    };

    // TODO:Sampled Spectrum
    template <typename Float>
    struct Spectrum final {
        Float r, g, b;
        Spectrum() noexcept = default;
        explicit Spectrum(Float v[4]) noexcept : r(v[0]), g(v[1]), b(v[2]) {}
        explicit Spectrum(Float rv, Float gv, Float bv) noexcept : r(rv), g(gv), b(bv) {}
        [[nodiscard]] Float luminosity() const noexcept {
            // TODO:more accurate weight
            return Float{ 0.298f * r.val + 0.612f * g.val * 0.117f * b.val };
        }
        Spectrum operator+(Spectrum rhs) const noexcept {
            return Spectrum{ r + rhs.r, g + rhs.g, b + rhs.b };
        }

        [[nodiscard]] bool valid() const noexcept {
            return r.val > 0.0f || g.val > 0.0f || b.val > 0.0f;
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
    };
    using Flux = Power<float>;
    using Irradiance = Ratio<Flux, Area<float>>;
    using Intensity = Ratio<Flux, SolidAngle<float>>;
    using Radiance = Ratio<Irradiance, SolidAngle<float>>;

    struct RGBW final {
        Spectrum<Radiance> radiance;
        Dimensionless<float> weight;
    };

    struct SensorNDCAffineTransform final {
        float ox, oy, sx, sy;
    };

    // type-safe
    class FullContextReserved;
    class RestrictedContextReserved;
    using FullContext = FullContextReserved*;
    using RestrictedContext = RestrictedContextReserved*;
    inline RestrictedContext decay(const FullContext context) {
        return reinterpret_cast<RestrictedContext>(context);
    }
    class SurfaceHandleReserved;
    using SurfaceHandle = const SurfaceHandleReserved*;
    class LightHandleReserved;
    using LightHandle = const LightHandleReserved*;
    class TraversalHandleReserved;
    using TraversalHandle = const TraversalHandleReserved*;
    class CallHandleReserved;
    using CallHandle = const CallHandleReserved*;
    class StatisticsHandleReserved;
    using StatisticsHandle = const StatisticsHandleReserved*;

    constexpr LightHandle environment = nullptr;

    using SensorFunc = void (*)(RestrictedContext context, const void* SBTData, const Vector2<float>& NDC, float u1, float u2,
                                RayInfo<FOR::World>& ray, Dimensionless<float>& weight);

    enum class BxDFPart : uint8_t { Reflection = 1, Transmission = 2, Diffuse = 4, Specular = 8, Glossy = 16, All = 31 };
    enum class TransportMode : uint8_t { Radiance, Importance };
    constexpr bool match(BxDFPart provide, BxDFPart require) {
        return (static_cast<uint32_t>(provide) & static_cast<uint32_t>(require)) == static_cast<uint32_t>(provide);
    }
    constexpr bool operator&(BxDFPart a, BxDFPart b) {
        return static_cast<bool>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    constexpr BxDFPart operator|(BxDFPart a, BxDFPart b) {
        return static_cast<BxDFPart>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    struct SurfaceSample final {
        Normal<float, FOR::Shading> wi;
        Spectrum<Dimensionless<float>> f;
        Dimensionless<float> pdf;
        BxDFPart part;
    };
    struct SurfaceStorage final {
        std::byte data[96];
    };

    // TODO:simplify interface
    // TODO:handle thin surface
    using SurfaceInitFunc = void (*)(RestrictedContext context, const void* SBTData, const Vector2<float>& texCoord,
                                     const Normal<float, FOR::Shading>& Ng, Face face, TransportMode mode, void* storage,
                                     bool& noSpecular);
    // TODO:provide 4 dimensions? (consider MDL)
    using SurfaceSampleFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                       const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng,
                                       BxDFPart require, float u1, float u2, SurfaceSample& sample);
    using SurfaceEvaluateFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                         const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                         const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                                         Spectrum<Dimensionless<float>>& f);
    using SurfacePdfFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                    const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                    const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf);

    using GeometryIntersectFunc = void (*)(RestrictedContext context, const void* SBTData, uint32_t primitiveID,
                                           const RayInfo<FOR::Local>& ray, float tNear, float& tFar, void* storage);
    using GeometryOccludeFunc = void (*)(RestrictedContext context, const void* SBTData, uint32_t primitiveID,
                                         const RayInfo<FOR::Local>& ray, float tNear, float tFar, bool& hit);
    using GeometryPostProcessFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                             SurfaceIntersectionInfo& info);
    using GeometrySampleFunc = void (*)(RestrictedContext context, const void* SBTData, const Point<Distance, FOR::World>& hit,
                                        float u1, float u2, Point<Distance, FOR::World>& src, Normal<float, FOR::World>& n,
                                        Dimensionless<float>& pdf);

    using RenderDriverFunc = void (*)(RestrictedContext context, const void* SBTData, const void* launchData,
                                      const Vector2<float>& point, const Spectrum<Radiance>& sample);
    using FilterFunc = void (*)(RestrictedContext context, const void* SBTData, float dx, float dy, Dimensionless<float>& weight);
    using IntegratorFunc = void (*)(FullContext context, const void* SBTData, RayInfo<FOR::World>& ray,
                                    Spectrum<Radiance>& sample);
    struct LightStorage final {
        std::byte data[32];
    };
    struct LightSample final {
        Normal<float, FOR::World> dir;  // src-hit
        Spectrum<Radiance> rad;
        Dimensionless<float> pdf;
        Distance distance;
    };
    struct LightSelectResult final {
        LightHandle light;
        Dimensionless<float> pdf;
        bool delta;
    };
    // TODO:spatial select?
    using LightSelectFunc = void (*)(RestrictedContext context, const void* SBTData, float u, LightSelectResult& select);

    // TODO:sample emission for BDPT and SPPM
    using LightInitFunc = void (*)(RestrictedContext context, const void* SBTData, void* storage);
    using LightSampleFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                     const Point<Distance, FOR::World>& hit, float u1, float u2, LightSample& sample);
    using LightEvaluateFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                       const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& n,
                                       const Normal<float, FOR::World>& dir, Spectrum<Radiance>& rad);
    using LightPdfFunc = void (*)(RestrictedContext context, const void* SBTData, const void* storage,
                                  const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& n,
                                  const Normal<float, FOR::World>& dir, Distance t, Dimensionless<float>& pdf);

    using SampleStartFunc = void (*)(const void* SBTData, uint32_t x, uint32_t y, uint32_t sample, uint64_t& idx,
                                     Vector2<float>& pos);
    using SampleGenerateFunc = void (*)(const void* SBTData, uint64_t idx, uint32_t dim, float& val);

    enum class TextureWrap : uint32_t { Repeat, Mirror };
    using TextureSampleFunc = void (*)(RestrictedContext context, const void* SBTData, const Vector2<float>& texCoord,
                                       Dimensionless<float>* sample);

    enum class TraceKind : unsigned char { Surface, AreaLight, Missing };

    struct TraceSurface final {
        SurfaceIntersectionInfo intersect;
        Transform<Distance, FOR::World, FOR::Local> transform;
        union {
            SurfaceHandle surface;
            LightHandle light;
        };
        Distance t;
    };

    struct TraceResult final {
        union {
            TraceSurface surface;
        };
        TraceKind kind;
    };

    struct CallInfo final {
        ptrdiff_t address;
        const void* SBTData;
    };

    extern "C" {
    void piperSurfaceInit(FullContext context, SurfaceHandle surface, const Vector2<float>& texCoord,
                          const Normal<float, FOR::Shading>& Ng, Face face, TransportMode mode, SurfaceStorage& storage,
                          bool& noSpecular);
    void piperSurfaceSample(FullContext context, SurfaceHandle surface, const SurfaceStorage& storage,
                            const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                            SurfaceSample& sample);
    void piperSurfaceEvaluate(FullContext context, SurfaceHandle surface, const SurfaceStorage& storage,
                              const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                              const Normal<float, FOR::Shading>& Ng, BxDFPart require, Spectrum<Dimensionless<float>>& f);
    void piperSurfacePdf(FullContext context, SurfaceHandle surface, const SurfaceStorage& storage,
                         const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                         const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf);

    void piperLightSelect(FullContext context, LightSelectResult& select);
    void piperLightInit(FullContext context, LightHandle light, LightStorage& storage);
    void piperLightSample(FullContext context, LightHandle light, const LightStorage& storage,
                          const Point<Distance, FOR::World>& hit, LightSample& sample);
    void piperLightEvaluate(FullContext context, LightHandle light, const LightStorage& storage,
                            const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& n,
                            const Normal<float, FOR::World>& dir, Spectrum<Radiance>& rad);
    void piperLightPdf(FullContext context, LightHandle light, const LightStorage& storage,
                       const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& n,
                       const Normal<float, FOR::World>& dir, Distance t, Dimensionless<float>& pdf);

    void piperTrace(FullContext context, const RayInfo<FOR::World>& ray, float minT, float maxT, TraceResult& result);
    bool piperOcclude(FullContext context, const RayInfo<FOR::World>& ray, float minT, float maxT);
    // TODO:terminate/ignore intersection

    float piperSample(FullContext context);

    void piperStatisticsUInt(RestrictedContext context, StatisticsHandle statistics, uint32_t val);
    void piperStatisticsBool(RestrictedContext context, StatisticsHandle statistics, bool val);
    void piperStatisticsFloat(RestrictedContext context, StatisticsHandle statistics, float val);
    void piperStatisticsTime(RestrictedContext context, StatisticsHandle statistics, uint64_t interval);
    void piperGetTime(RestrictedContext context, uint64_t& val);

    void piperQueryCall(RestrictedContext context, CallHandle call, CallInfo& info);
    void piperQueryTransform(RestrictedContext context, TraversalHandle traversal,
                             Transform<Distance, FOR::Local, FOR::World>& transform);
    Time<float> piperQueryTime(RestrictedContext context);

    // TODO:better interface
    // only for debug
    // TODO:context
    void piperPrintFloat(RestrictedContext context, const char* name, float val);

    void piperFloatAtomicAdd(float& x, float y);
    }
    // TODO:better interface? consider optix
    template <typename Func, typename... Args>
    void piperCall(RestrictedContext context, const CallHandle call, Args&&... args) {
        CallInfo info;
        piperQueryCall(context, call, info);
        reinterpret_cast<Func>(info.address)(context, info.SBTData, std::forward<Args>(args)...);
    }

    class TimeProfiler final {
    private:
        RestrictedContext mContext;
        StatisticsHandle mStatistics;
        uint64_t mBegin;

    public:
        TimeProfiler(const RestrictedContext context, const StatisticsHandle statistics)
            : mContext(context), mStatistics(statistics) {
            piperGetTime(mContext, mBegin);
        }
        ~TimeProfiler() {
            uint64_t end;
            piperGetTime(mContext, end);
            piperStatisticsTime(mContext, mStatistics, end - mBegin);
        }
    };
}  // namespace Piper
