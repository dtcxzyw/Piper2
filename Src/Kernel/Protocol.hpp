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
    template <FOR ref>
    struct RayInfo final {
        Point<Distance, ref> origin;
        Normal<float, ref> direction;
        float t;
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

    struct FullContext;
    struct RestrictedContext;

    inline RestrictedContext* decay(FullContext* context) {
        return reinterpret_cast<RestrictedContext*>(context);
    }

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
        Spectrum& operator+=(Spectrum rhs) noexcept {
            r = r + rhs.r, g = g + rhs.g, b = b + rhs.b;
            return *this;
        }
    };
    using Flux = Power<float>;
    using Irradiance = Ratio<Flux, Area<float>>;
    using Intensity = Ratio<Flux, SolidAngle<float>>;
    using Radiance = Ratio<Irradiance, SolidAngle<float>>;

    struct SensorNDCAffineTransform final {
        float ox, oy, sx, sy;
    };

    using SensorFunc = void (*)(RestrictedContext* context, const void* SBTData, const Vector2<float>& NDC, float u1, float u2,
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
    using SurfaceInitFunc = void (*)(RestrictedContext* context, const void* SBTData, float t, const Vector2<float>& texCoord,
                                     const Normal<float, FOR::Shading>& Ng, Face face, TransportMode mode, void* storage,
                                     bool& noSpecular);
    // TODO:provide 4 dimensions? (consider MDL)
    using SurfaceSampleFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                       const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng,
                                       BxDFPart require, float u1, float u2, SurfaceSample& sample);
    using SurfaceEvaluateFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                         const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                         const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                                         Spectrum<Dimensionless<float>>& f);
    using SurfacePdfFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                    const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                    const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf);

    using GeometryIntersectFunc = void (*)(RestrictedContext* context, const void* SBTData, uint32_t primitiveID,
                                           const RayInfo<FOR::Local>& ray, float tNear, float& tFar, void* storage);
    using GeometryOccludeFunc = void (*)(RestrictedContext* context, const void* SBTData, uint32_t primitiveID,
                                         const RayInfo<FOR::Local>& ray, float tNear, float tFar, bool& hit);
    using GeometryPostProcessFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage, float t,
                                             SurfaceIntersectionInfo& info);

    using RenderDriverFunc = void (*)(RestrictedContext* context, const void* SBTData, const Vector2<float>& point,
                                      const Spectrum<Radiance>& sample);
    using IntegratorFunc = void (*)(FullContext* context, const void* SBTData, RayInfo<FOR::World>& ray,
                                    Spectrum<Radiance>& sample);
    struct LightStorage final {
        std::byte data[32];
    };
    struct LightSample final {
        Normal<float, FOR::World> dir;  // light.origin-hit
        Spectrum<Radiance> rad;
        Dimensionless<float> pdf;
    };
    struct LightSelectResult final {
        uint64_t light;
        Dimensionless<float> pdf;
        bool delta;
    };
    // TODO:spatial select?
    using LightSelectFunc = void (*)(RestrictedContext* context, const void* SBTData, float u, LightSelectResult& select);
    using LightInitFunc = void (*)(RestrictedContext* context, const void* SBTData, float t, void* storage);
    using LightSampleFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                     const Point<Distance, FOR::World>& hit, float u1, float u2, LightSample& sample);
    using LightEvaluateFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                       const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                                       Spectrum<Radiance>& rad);
    using LightPdfFunc = void (*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                  const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                                  Dimensionless<float>& pdf);

    using SampleStartFunc = void (*)(const void* SBTData, uint32_t x, uint32_t y, uint32_t sample, uint64_t& idx,
                                     Vector2<float>& pos);
    using SampleGenerateFunc = void (*)(const void* SBTData, uint64_t idx, uint32_t dim, float& val);

    enum class TextureWrap : uint32_t { Repeat, Mirror };
    using TextureSampleFunc = void (*)(RestrictedContext* context, const void* SBTData, float t, const Vector2<float>& texCoord,
                                       Dimensionless<float>* sample);

    enum class TraceKind : unsigned char { Surface, Missing };
    struct TraceSurface final {
        SurfaceIntersectionInfo intersect;
        Transform<Distance, FOR::World, FOR::Local> transform;
        uint64_t instance;
        Distance t;
    };
    struct TraceResult final {
        union {
            TraceSurface surface;
        };
        TraceKind kind;
    };

    struct CallInfo final {
        uint64_t address;
        const void* SBTData;
    };

    // TODO:replace uint64_t with unsigned<ptrdiff_t>
    extern "C" {
    void piperSurfaceInit(FullContext* context, uint64_t instance, float t, const Vector2<float>& texCoord,
                          const Normal<float, FOR::Shading>& Ng, Face face, TransportMode mode, SurfaceStorage& storage,
                          bool& noSpecular);
    void piperSurfaceSample(FullContext* context, uint64_t instance, const SurfaceStorage& storage,
                            const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                            SurfaceSample& sample);
    void piperSurfaceEvaluate(FullContext* context, uint64_t instance, const SurfaceStorage& storage,
                              const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                              const Normal<float, FOR::Shading>& Ng, BxDFPart require, Spectrum<Dimensionless<float>>& f);
    void piperSurfacePdf(FullContext* context, uint64_t instance, const SurfaceStorage& storage,
                         const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                         const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf);

    void piperLightSelect(FullContext* context, LightSelectResult& select);
    void piperLightInit(FullContext* context, uint64_t light, float t, LightStorage& storage);
    void piperLightSample(FullContext* context, uint64_t light, const LightStorage& storage,
                          const Point<Distance, FOR::World>& hit, LightSample& sample);
    void piperLightEvaluate(FullContext* context, uint64_t light, const LightStorage& storage,
                            const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                            Spectrum<Radiance>& rad);
    void piperLightPdf(FullContext* context, uint64_t light, const LightStorage& storage, const Point<Distance, FOR::World>& hit,
                       const Normal<float, FOR::World>& dir, Dimensionless<float>& pdf);

    void piperTrace(FullContext* context, const RayInfo<FOR::World>& ray, float minT, float maxT, TraceResult& result);
    bool piperOcclude(FullContext* context, const RayInfo<FOR::World>& ray, float minT, float maxT);
    // TODO:terminate/ignore intersection

    float piperSample(FullContext* context);

    void piperStatisticsUInt(RestrictedContext* context, uint32_t id, uint32_t val);
    void piperStatisticsBool(RestrictedContext* context, uint32_t id, bool val);
    void piperStatisticsFloat(RestrictedContext* context, uint32_t id, float val);
    void piperStatisticsTime(RestrictedContext* context, uint32_t id, uint64_t interval);
    void piperGetTime(RestrictedContext* context, uint64_t& val);

    void piperQueryCall(RestrictedContext* context, uint32_t id, CallInfo& info);

    // TODO:better interface
    // only for debug
    // TODO:context
    void piperPrintFloat(RestrictedContext* context, const char* name, float val);
    }
    // TODO:better interface? consider optix
    template <typename Func, typename... Args>
    void piperCall(RestrictedContext* context, const uint32_t id, Args&&... args) {
        CallInfo call;
        piperQueryCall(context, id, call);
        reinterpret_cast<Func>(call.address)(context, call.SBTData, std::forward<Args>(args)...);
    }

    class TimeProfiler final {
    private:
        RestrictedContext* mContext;
        uint32_t mID;
        uint64_t mBegin;

    public:
        TimeProfiler(RestrictedContext* context, const uint32_t id) : mContext(context), mID(id) {
            piperGetTime(mContext, mBegin);
        }
        ~TimeProfiler() {
            uint64_t end;
            piperGetTime(mContext, end);
            piperStatisticsTime(mContext, mID, end - mBegin);
        }
    };
}  // namespace Piper
