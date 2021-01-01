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
        float t;
    };

    enum class Face { Front, Back };

    struct BuiltinTriangleBuffer {
        const uint32_t* index;
        const Vector2<float>* texCoord;
        const Vector<float, FOR::Local>* Ns;
        const Vector<float, FOR::Local>* Ts;
    };

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
        Normal<float, FOR::Local> T, B, N, Ng;
        Vector2<float> texCoord;

        [[nodiscard]] auto local2Shading(Normal<float, FOR::Local> v) const noexcept {
            return Normal<float, FOR::Shading>{ Vector<Dimensionless<float>, FOR::Shading>{ dot(T, v), dot(B, v), dot(N, v) },
                                                Unchecked{} };
        }

        [[nodiscard]] auto shading2Local(Normal<float, FOR::Shading> v) const noexcept {
            return Normal<float, FOR::Local>{ Vector<Dimensionless<float>, FOR::Local>{
                                                  dot(T.x, v.x) + dot(B.x, v.y) + dot(N.x, v.z),
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

    using SensorFunc = void (*)(RestrictedContext* context, const void* SBTData, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                const SensorNDCAffineTransform& transform, RayInfo& ray, Vector2<float>& point);

    enum class BxDFPart : uint32_t { Reflection = 1, Refraction = 2, Diffuse = 4, Specular = 8, Glossy = 16, All = 31 };
    constexpr bool match(BxDFPart provide, BxDFPart require) {
        return (static_cast<uint32_t>(provide) & static_cast<uint32_t>(require)) == static_cast<uint32_t>(provide);
    }
    constexpr bool operator&(BxDFPart a, BxDFPart b) {
        return static_cast<bool>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
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
        std::byte data[64];
    };

    // TODO:simplify interface
    using SurfaceInitFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, float t,
                                            const Vector2<float>& texCoord, const Normal<float, FOR::Shading>& Ng, void* storage,
                                            bool& noSpecular);
    using SurfaceSampleFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                              const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng,
                                              BxDFPart require, SurfaceSample& sample);
    using SurfaceEvaluateFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                                const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                                const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                                                Spectrum<Dimensionless<float>>& f);
    using SurfacePdfFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                           const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                           const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf);

    using GeometryFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const HitInfo& hit, float t,
                                         SurfaceIntersectionInfo& info);
    using RenderDriverFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const Vector2<float>& point,
                                             const Spectrum<Radiance>& sample);
    using IntegratorFunc = void(PIPER_CC*)(FullContext* context, const void* SBTData, RayInfo& ray, Spectrum<Radiance>& sample);
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
    using LightSelectFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, LightSelectResult& select);
    using LightInitFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, float t, void* storage);
    using LightSampleFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                            const Point<Distance, FOR::World>& hit, LightSample& sample);
    using LightEvaluateFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                              const Point<Distance, FOR::World>& lightSourceHit,
                                              const Normal<float, FOR::World>& dir, Spectrum<Radiance>& rad);
    using LightPdfFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, const void* storage,
                                         const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                                         Dimensionless<float>& pdf);

    using SampleFunc = void(PIPER_CC*)(const void* SBTData, uint32_t x, uint32_t y, uint32_t s, float* samples);

    enum class TextureWrap : uint32_t { Repeat, Mirror };
    using TextureSampleFunc = void(PIPER_CC*)(RestrictedContext* context, const void* SBTData, float t,
                                              const Vector2<float>& texCoord, Dimensionless<float>* sample);

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
    void PIPER_CC piperSurfaceInit(FullContext* context, uint64_t instance, float t, const Vector2<float>& texCoord,
                                   const Normal<float, FOR::Shading>& Ng, SurfaceStorage& storage, bool& noSpecular);
    void PIPER_CC piperSurfaceSample(FullContext* context, uint64_t instance, const SurfaceStorage& storage,
                                     const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng,
                                     BxDFPart require, SurfaceSample& sample);
    void PIPER_CC piperSurfaceEvaluate(FullContext* context, uint64_t instance, const SurfaceStorage& storage,
                                       const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                       const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                                       Spectrum<Dimensionless<float>>& f);
    void PIPER_CC piperSurfacePdf(FullContext* context, uint64_t instance, const SurfaceStorage& storage,
                                  const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                  const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf);

    void PIPER_CC piperLightSelect(FullContext* context, LightSelectResult& select);
    void PIPER_CC piperLightInit(FullContext* context, uint64_t light, float t, LightStorage& storage);
    void PIPER_CC piperLightSample(FullContext* context, uint64_t light, const LightStorage& storage,
                                   const Point<Distance, FOR::World>& hit, LightSample& sample);
    void PIPER_CC piperLightEvaluate(FullContext* context, uint64_t light, const LightStorage& storage,
                                     const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                                     Spectrum<Radiance>& rad);
    void PIPER_CC piperLightPdf(FullContext* context, uint64_t light, const LightStorage& storage,
                                const Point<Distance, FOR::World>& hit, const Normal<float, FOR::World>& dir,
                                Dimensionless<float>& pdf);

    void PIPER_CC piperTrace(FullContext* context, const RayInfo& ray, float minT, float maxT, TraceResult& result);
    void PIPER_CC piperOcclude(FullContext* context, const RayInfo& ray, float minT, float maxT, bool& result);
    // TODO:terminate/ignore intersection
    // TODO:need FullContext
    float PIPER_CC piperSample(RestrictedContext* context);
    // TODO:debug interface
    void PIPER_CC piperPrintMessage(RestrictedContext* context, const char* msg);
    void PIPER_CC piperPrintFloat(RestrictedContext* context, const char* desc, float val);
    void PIPER_CC piperPrintUint(RestrictedContext* context, const char* desc, uint32_t val);

    void PIPER_CC piperQueryCall(RestrictedContext* context, uint32_t id, CallInfo& info);
    }
    // TODO:better interface? consider optix
    template <typename Func, typename... Args>
    void piperCall(RestrictedContext* context, const uint32_t id, Args&&... args) {
        CallInfo call;
        piperQueryCall(context, id, call);
        reinterpret_cast<Func>(call.address)(context, call.SBTData, std::forward<Args>(args)...);
    }
}  // namespace Piper
