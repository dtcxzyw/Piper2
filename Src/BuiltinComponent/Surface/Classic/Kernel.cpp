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

#include "../../../Kernel/Sampling.hpp"
#include "Shared.hpp"

#include <algorithm>

namespace Piper {
    using Vec = Normal<float, FOR::Shading>;
    using BxDFValue = Spectrum<Dimensionless<float>>;
    using PDFValue = Dimensionless<float>;

    /*
    auto diffuse(Spectrum<Dimensionless<float>> baseColor, Dimensionless<float> roughness, Dimensionless<float> thetaV,
                 Dimensionless<float> thetaD, Dimensionless<float> thetaL) {
        auto invPow5 = [](Dimensionless<float> x) {
            auto inv = Dimensionless<float>{ 1.0f } - x;
            auto inv2 = inv * inv;
            return inv * inv2 * inv2;
        };
        auto F_L = invPow5(thetaL);
        auto F_V = invPow5(thetaV);
        auto R_R = Dimensionless<float>{ 2.0f } * roughness * thetaD * thetaD;
        auto lambert = (Dimensionless<float>{ 1.0f } - Dimensionless<float>{ 0.5f } * F_L) *
            (Dimensionless<float>{ 1.0f } - Dimensionless<float>{ 0.5f } * F_V);
        auto retro_reflection = R_R * (F_L + F_V + F_L * F_V * (R_R - Dimensionless<float>{ 1.0f }));
        auto factor = (lambert + retro_reflection) / Constants::pi<Dimensionless<float>>;
        return baseColor * factor;
    }
    */
    extern "C" void PIPER_CC blackBodyInit(RestrictedContext*, const void*, float, const Vector2<float>&,
                                           const Normal<float, FOR::Shading>&, Face, TransportMode, void*, bool& noSpecular) {
        noSpecular = false;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&blackBodyInit)>);
    extern "C" void PIPER_CC blackBodySample(RestrictedContext*, const void*, const void*, const Vec&,
                                             const Normal<float, FOR::Shading>&, BxDFPart, SurfaceSample&) {}
    static_assert(std::is_same_v<SurfaceSampleFunc, decltype(&blackBodySample)>);
    extern "C" void PIPER_CC blackBodyEvaluate(RestrictedContext*, const void*, const void*, const Vec&, const Vec&,
                                               const Normal<float, FOR::Shading>&, BxDFPart, BxDFValue& f) {
        f = Spectrum<Dimensionless<float>>{ { 0.0f }, { 0.0f }, { 0.0f } };
    }
    static_assert(std::is_same_v<SurfaceEvaluateFunc, decltype(&blackBodyEvaluate)>);
    extern "C" void PIPER_CC blackBodyPdf(RestrictedContext*, const void*, const void*, const Vec&, const Vec&,
                                          const Normal<float, FOR::Shading>&, BxDFPart, Dimensionless<float>& pdf) {
        pdf = Dimensionless<float>{ 0.0f };
    }
    static_assert(std::is_same_v<SurfacePdfFunc, decltype(&blackBodyPdf)>);

    template <typename T>
    struct DefaultBxDFHelper {
        void sample(const float u1, const float u2, const Vec& wo, SurfaceSample& res) const {
            res.wi = sampleCosineHemisphere(u1, u2);
            auto&& self = *static_cast<const T*>(this);
            res.f = self.evaluate(wo, res.wi);
            res.pdf = pdf(wo, res.wi);
            res.part = T::part;
        }

        [[nodiscard]] PDFValue pdf(const Vec& wo, const Vec& wi) const {
            return (wo.z * wi.z).val > 0.0f ? PDFValue{ std::abs(wi.z.val) * Constants::invPi<float> } : PDFValue{ 0.0f };
        }
    };

    class LambertianReflection final : public DefaultBxDFHelper<LambertianReflection> {
    private:
        BxDFValue mReflection;

    public:
        static constexpr BxDFPart part = BxDFPart::Diffuse | BxDFPart::Reflection;
        explicit LambertianReflection(const BxDFValue& reflection) : mReflection(reflection) {}

        [[nodiscard]] BxDFValue evaluate(const Vec&, const Vec&) const {
            return mReflection * Dimensionless<float>{ Constants::invPi<float> };
        }
    };

    class OrenNayar final : public DefaultBxDFHelper<OrenNayar> {
    private:
        BxDFValue mReflection;
        Dimensionless<float> mA, mB;

    public:
        static constexpr BxDFPart part = BxDFPart::Diffuse | BxDFPart::Reflection;
        // sigma:[0,1]
        OrenNayar(const BxDFValue& reflection, Dimensionless<float> sigma) : mReflection(reflection) {
            sigma = sigma * Dimensionless<float>{ Constants::pi<float> };
            const auto sigma2 = sigma * sigma;
            mA = Dimensionless<float>{ 1.0f } -
                (sigma2 / (Dimensionless<float>{ 2.0f } * (sigma2 + Dimensionless<float>{ 0.33f })));
            mB = Dimensionless<float>{ 0.45f } * sigma2 / (sigma2 + Dimensionless<float>{ 0.09f });
        }

        [[nodiscard]] BxDFValue evaluate(const Vec& wo, const Vec& wi) const {
            const auto sinThetaI = sqrtSafe(Dimensionless<float>{ 1.0f } - wi.z * wi.z);
            const auto sinThetaO = sqrtSafe(Dimensionless<float>{ 1.0f } - wo.z * wo.z);
            if(sinThetaI.val < 1e-4f || sinThetaO.val < 1e-4f)
                return {};
            const auto cosPhiI =
                Dimensionless<float>{ sinThetaI.val == 0.0f ? 1.0f :
                                                              std::fmin(1.0f, std::fmax(-1.0f, wi.x.val / sinThetaI.val)) };
            const auto sinPhiI =
                Dimensionless<float>{ sinThetaI.val == 0.0f ? 0.0f :
                                                              std::fmin(1.0f, std::fmax(-1.0f, wi.y.val / sinThetaI.val)) };
            const auto cosPhiO =
                Dimensionless<float>{ sinThetaO.val == 0.0f ? 1.0f :
                                                              std::fmin(1.0f, std::fmax(-1.0f, wo.x.val / sinThetaO.val)) };
            const auto sinPhiO =
                Dimensionless<float>{ sinThetaO.val == 0.0f ? 0.0f :
                                                              std::fmin(1.0f, std::fmax(-1.0f, wo.y.val / sinThetaO.val)) };
            const auto maxCos = Dimensionless<float>{ std::fmax(0.0f, (cosPhiI * cosPhiO + sinPhiI * sinPhiO).val) };

            Dimensionless<float> sinAlpha, tanBeta;
            if(abs(wi.z).val > abs(wo.z).val) {
                sinAlpha = sinThetaO;
                tanBeta = sinThetaI / abs(wi.z);
            } else {
                sinAlpha = sinThetaI;
                tanBeta = sinThetaO / abs(wo.z);
            }
            return mReflection * (Dimensionless<float>{ Constants::invPi<float> } * (mA + mB * maxCos * sinAlpha * tanBeta));
        }
    };

    using Mask = uint8_t;

    void sampleN(Mask mask, uint32_t i, float u1, float u2, const Vec& wo, BxDFPart require, SurfaceSample&) {
        // TODO:assertion failed
    }

    template <typename First, typename... BxDFs>
    void sampleN(const Mask mask, uint32_t i, float u1, float u2, const Vec& wo, BxDFPart require, SurfaceSample& sample,
                 const First& current, const BxDFs&... bxdfs) {
        const auto flag = (mask & 1) && match(First::part, require);
        if(flag && i == 0)
            current.sample(u1, u2, wo, sample);
        else
            sampleN(mask >> 1, i - flag, u1, u2, wo, require, sample, bxdfs...);
    }
    void pdfN(Mask, uint32_t, const Vec&, const Vec&, BxDFPart, PDFValue&) {}
    template <typename First, typename... BxDFs>
    void pdfN(const Mask mask, uint32_t i, const Vec& wo, const Vec& wi, BxDFPart require, PDFValue& pdf, const First& current,
              const BxDFs&... bxdfs) {
        const auto flag = (mask & 1) && match(First::part, require);
        if(flag && i != 0) {
            pdf = pdf + current.pdf(wo, wi);
        }
        pdfN(mask >> 1, i - flag, wo, wi, require, pdf, bxdfs...);
    }
    void evaluateN(Mask, uint32_t, const Vec&, const Vec&, BxDFPart, BxDFPart, BxDFValue&) {}
    template <typename First, typename... BxDFs>
    void evaluateN(const Mask mask, uint32_t i, const Vec& wo, const Vec& wi, BxDFPart require, BxDFPart addition, BxDFValue& f,
                   const First& current, const BxDFs&... bxdfs) {
        const auto flag = (mask & 1) && match(First::part, require);
        if(flag && i != 0 && (addition & First::part)) {
            f = f + current.evaluate(wo, wi);
        }
        evaluateN(mask >> 1, i - flag, wo, wi, require, addition, f, bxdfs...);
    }

    uint32_t countN(Mask, BxDFPart) {
        return 0;
    }
    template <typename First, typename... BxDFs>
    uint32_t countN(const Mask mask, const BxDFPart require, const First&, const BxDFs&... bxdfs) {
        return ((mask & 1) && match(First::part, require)) + countN(mask >> 1, require, bxdfs...);
    }

    template <typename... BxDFs>
    void sample(const Mask mask, float u1, float u2, const Vec& wo, const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                SurfaceSample& sample, const BxDFs&... bxdfs) {
        const auto count = countN(mask, require, bxdfs...);
        if(count == 0)
            return;
        const auto select = std::min(static_cast<uint32_t>(std::floor(u1 * static_cast<float>(count))), count - 1);
        u1 = u1 * static_cast<float>(count) - static_cast<float>(select);
        sampleN(mask, select, u1, u2, wo, require, sample, bxdfs...);
        if(static_cast<uint32_t>(sample.part) == 0)
            return;
        if(count > 1 && !(sample.part & BxDFPart::Specular)) {
            const auto addition = ((dot(wo, Ng) * dot(sample.wi, Ng)).val > 0.0f ? BxDFPart::Reflection : BxDFPart::Transmission);
            evaluateN(mask, select, wo, sample.wi, require, addition, sample.f, bxdfs...);
            pdfN(mask, select, wo, sample.wi, require, sample.pdf, bxdfs...);
        }
        if(count > 1)
            sample.pdf = sample.pdf * Dimensionless<float>{ 1.0f / count };
    }

#define KERNEL_FUNCTION_GROUP(PREFIX, BSDF, ...)                                                                             \
    extern "C" void PIPER_CC PREFIX##Sample(RestrictedContext* context, const void*, const void* storage, const Vec& wo,     \
                                            const Normal<float, FOR::Shading>& Ng, const BxDFPart require,                   \
                                            SurfaceSample& sample) {                                                         \
        const auto* bsdf = static_cast<const BSDF*>(storage);                                                                \
        Piper::sample(bsdf->mask, piperSample(context), piperSample(context), wo, Ng, require, sample, __VA_ARGS__);         \
    }                                                                                                                        \
    static_assert(std::is_same_v<SurfaceSampleFunc, decltype(&PREFIX##Sample)>);                                             \
                                                                                                                             \
    extern "C" void PIPER_CC PREFIX##Evaluate(RestrictedContext*, const void*, const void* storage, const Vec& wo,           \
                                              const Vec& wi, const Normal<float, FOR::Shading>& Ng, const BxDFPart require,  \
                                              BxDFValue& f) {                                                                \
        const auto* bsdf = static_cast<const BSDF*>(storage);                                                                \
        const auto addition = ((dot(wo, Ng) * dot(wi, Ng)).val > 0.0f ? BxDFPart::Reflection : BxDFPart::Transmission);      \
        f = {};                                                                                                              \
        evaluateN(bsdf->mask, std::numeric_limits<uint32_t>::max(), wo, wi, require, addition, f, __VA_ARGS__);              \
    }                                                                                                                        \
    static_assert(std::is_same_v<SurfaceEvaluateFunc, decltype(&PREFIX##Evaluate)>);                                         \
                                                                                                                             \
    extern "C" void PIPER_CC PREFIX##Pdf(RestrictedContext*, const void*, const void* storage, const Vec& wo, const Vec& wi, \
                                         const Normal<float, FOR::Shading>&, const BxDFPart require, PDFValue& pdf) {        \
        const auto* bsdf = static_cast<const BSDF*>(storage);                                                                \
        pdf = {};                                                                                                            \
        pdfN(bsdf->mask, std::numeric_limits<uint32_t>::max(), wo, wi, require, pdf, __VA_ARGS__);                           \
    }                                                                                                                        \
    static_assert(std::is_same_v<SurfacePdfFunc, decltype(&PREFIX##Pdf)>);

    struct MatteBSDF final {
        union {
            LambertianReflection lambertian;
            OrenNayar orenNayar;
        };
        Mask mask;
    };

    KERNEL_FUNCTION_GROUP(matte, MatteBSDF, bsdf->orenNayar, bsdf->lambertian)

    extern "C" void PIPER_CC matteInit(RestrictedContext* context, const void* SBTData, float t, const Vector2<float>& texCoord,
                                       const Normal<float, FOR::Shading>&, Face, TransportMode, void* storage, bool& noSpecular) {
        const auto* data = static_cast<const MatteData*>(SBTData);
        auto* bsdf = static_cast<MatteBSDF*>(storage);
        static_assert(sizeof(MatteBSDF) <= sizeof(SurfaceStorage));
        Dimensionless<float> diffuse[4];
        piperCall<TextureSampleFunc>(context, data->diffuseTexture, t, texCoord, diffuse);
        const auto reflection = Spectrum<Dimensionless<float>>{ diffuse };
        Dimensionless<float> roughness;
        piperCall<TextureSampleFunc>(context, data->roughnessTexture, t, texCoord, &roughness);

        if(roughness.val > 0.0f) {
            bsdf->mask = 0b01;
            bsdf->orenNayar = OrenNayar{ reflection, roughness };
        } else {
            bsdf->mask = 0b10;
            bsdf->lambertian = LambertianReflection{ reflection };
        }
        noSpecular = true;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&matteInit)>);

    struct FresnelDielectric final {
        Dimensionless<float> etaI, etaT;
        Dimensionless<float> operator()(Dimensionless<float> cosThetaI) const {
            cosThetaI = { std::fmax(-1.0f, std::fmin(1.0f, cosThetaI.val)) };
            const auto sinThetaI = sqrtSafe(Dimensionless<float>{ 1.0f } - cosThetaI * cosThetaI);
            const auto sinThetaT = etaI / etaT * sinThetaI;
            if(sinThetaT.val >= 1.0f)
                return { 1.0f };
            const auto cosThetaT = sqrtSafe(Dimensionless<float>{ 1.0f } - sinThetaT * sinThetaI);
            const auto p1 = etaT * cosThetaI, p2 = etaI * cosThetaT;
            const auto parallelPart = (p1 - p2) / (p1 + p2);
            const auto p3 = etaI * cosThetaI, p4 = etaT * cosThetaT;
            const auto perpendicularPart = (p3 - p4) / (p3 + p4);
            return Dimensionless<float>{ 0.5f } * (parallelPart * parallelPart + perpendicularPart * perpendicularPart);
        }
    };

    // TrowbridgeReitz
    class MicrofacetDistribution final {
    private:
        Dimensionless<float> mAlphaX, mAlphaY;

        [[nodiscard]] Vector2<Dimensionless<float>> sample11(Dimensionless<float> cosTheta, const float u1,
                                                             const float u2) const {
            if(cosTheta.val > 1.0f - 1e-5f) {
                const auto r = Dimensionless<float>{ sqrt(u1 / (1 - u1)) };
                const auto phi = Radian<float>{ Constants::twoPi<float> * u2 };
                return { r * cos(phi), r * sin(phi) };
            }

            const auto sinTheta = sqrtSafe(Dimensionless<float>{ 1.0f } - cosTheta * cosTheta);
            const auto tanTheta = sinTheta / cosTheta;
            const auto tan2Theta = tanTheta * tanTheta;
            const auto g1 = 2.0f / (1.0f + std::sqrt(1.0f + tan2Theta.val));

            const auto a = Dimensionless<float>{ 2.0f * u1 / g1 - 1.0f };
            const auto a2 = a * a;
            const auto v = Dimensionless<float>{ std::fmin(1e10f, 1.0f / (a2.val - 1.0f)) };
            const auto d = sqrtSafe(tan2Theta * v * v - (a2 - tan2Theta) * v);
            const auto p = tanTheta * v;
            const auto p1 = p - d;
            const auto p2 = p + d;
            const auto x = a.val < 0.0f || p2.val > 1.0f / tanTheta.val ? p1 : p2;

            const auto s = u2 > 0.5f ? 1.0f : -1.0f;
            const auto nu2 = (u2 > 0.5f ? 2.0f : -2.0f) * (u2 - 0.5f);
            const auto z = (nu2 * (nu2 * (nu2 * 0.27385f - 0.73369f) + 0.46341f)) /
                (nu2 * (nu2 * (nu2 * 0.093073f + 0.309420f) - 1.000000f) + 0.597999f);
            const auto y = Dimensionless<float>{ s * z * std::sqrt(1.0f + x.val * x.val) };
            return { x, y };
        }

    public:
        MicrofacetDistribution(const Dimensionless<float> alphaX, const Dimensionless<float> alphaY)
            : mAlphaX(alphaX), mAlphaY(alphaY) {}
        [[nodiscard]] Dimensionless<float> lambda(const Vec& w) const {
            const auto cos2Theta = w.z * w.z;
            const auto sin2Theta = Dimensionless<float>{ std::fmax(0.0f, 1.0f - cos2Theta.val) };
            const auto tan2Theta = sin2Theta / cos2Theta;
            if(std::isinf(tan2Theta.val))
                return { 0.0f };
            const auto sinTheta = Dimensionless<float>{ std::sqrt(sin2Theta.val) };
            const auto cosPhi =
                Dimensionless<float>{ sinTheta.val == 0.0f ? 1.0f : std::fmin(1.0f, std::fmax(-1.0f, w.x.val / sinTheta.val)) };
            const auto p1 = cosPhi * mAlphaX;
            const auto sinPhi =
                Dimensionless<float>{ sinTheta.val == 0.0f ? 0.0f : std::fmin(1.0f, std::fmax(-1.0f, w.y.val / sinTheta.val)) };
            const auto p2 = sinPhi * mAlphaY;
            const auto sqr = (p1 * p1 + p2 * p2) * tan2Theta;
            return Dimensionless<float>{ (-1.0f + std::sqrt(1.0f + sqr.val)) * 0.5f };
        }
        [[nodiscard]] Dimensionless<float> evaluateD(const Vec& wh) const {
            const auto cos2Theta = wh.z * wh.z;
            const auto sin2Theta = Dimensionless<float>{ std::fmax(0.0f, 1.0f - cos2Theta.val) };
            const auto tan2Theta = sin2Theta / cos2Theta;
            if(std::isinf(tan2Theta.val))
                return { 0.0f };
            const auto cos4Theta = cos2Theta * cos2Theta;
            const auto sinTheta = Dimensionless<float>{ std::sqrt(sin2Theta.val) };
            const auto cosPhi =
                Dimensionless<float>{ sinTheta.val == 0.0f ? 1.0f : std::fmin(1.0f, std::fmax(-1.0f, wh.x.val / sinTheta.val)) };
            const auto p1 = cosPhi / mAlphaX;
            const auto sinPhi =
                Dimensionless<float>{ sinTheta.val == 0.0f ? 0.0f : std::fmin(1.0f, std::fmax(-1.0f, wh.y.val / sinTheta.val)) };
            const auto p2 = sinPhi / mAlphaY;
            const auto e = Dimensionless<float>{ 1.0f } + (p1 * p1 + p2 * p2) * tan2Theta;
            return Dimensionless<float>{ Constants::invPi<float> } / (mAlphaX * mAlphaY * cos4Theta * e * e);
        }
        // TODO:comment
        [[nodiscard]] Vec sampleWh(const Vec& wi, const float u1, const float u2) const {
            const auto stretched = Vec{ Vector<Dimensionless<float>, FOR::Shading>{ mAlphaX * wi.x, mAlphaY * wi.y, wi.z } };

            const auto slope = sample11(stretched.z, u1, u2);

            const auto sinTheta = sqrtSafe(Dimensionless<float>{ 1.0f } - stretched.z * stretched.z);
            const auto cosPhi =
                Dimensionless<float>{ sinTheta.val == 0.0f ? 1.0f :
                                                             std::fmin(1.0f, std::fmax(-1.0f, stretched.x.val / sinTheta.val)) };
            const auto sinPhi =
                Dimensionless<float>{ sinTheta.val == 0.0f ? 0.0f :
                                                             std::fmin(1.0f, std::fmax(-1.0f, stretched.y.val / sinTheta.val)) };

            const auto rotatedSlope =
                Vector2<Dimensionless<float>>{ cosPhi * slope.x - sinPhi * slope.y, sinPhi * slope.x + cosPhi * slope.y };

            return Vec{ Vector<Dimensionless<float>, FOR::Shading>{ -rotatedSlope.x * mAlphaX, -rotatedSlope.y * mAlphaY,
                                                                    Dimensionless<float>{ 1.0f } } };
        }
        static Dimensionless<float> remapRoughness(const Dimensionless<float> roughness) {
            const auto x = std::log(std::fmax(1e-3f, roughness.val));
            return { std::fmax(1e-3f, 1.62142f + (0.819955f + (0.1734f + (0.0171201f + 0.000640711f * x) * x) * x) * x) };
        }
    };

    template <typename Fresnel>
    class MicrofacetReflection final {
    private:
        BxDFValue mReflection;
        Fresnel mFresnel;
        MicrofacetDistribution mDistribution;

    public:
        static constexpr BxDFPart part = BxDFPart::Glossy | BxDFPart::Reflection;
        explicit MicrofacetReflection(const BxDFValue& reflection, const Fresnel& fresnel,
                                      const MicrofacetDistribution& distribution)
            : mReflection(reflection), mFresnel(fresnel), mDistribution(distribution) {}

        [[nodiscard]] BxDFValue evaluate(const Vec& wo, const Vec& wi) const {
            const auto wh = halfVector(wo, wi);
            return mReflection *
                (mFresnel(dot(wi, wh)) *
                 (mDistribution.evaluateD(wh) /
                  (Dimensionless<float>{ 4.0f } *
                   (Dimensionless<float>{ 1.0f } + mDistribution.lambda(wo) + mDistribution.lambda(wi)) * abs(wo.z * wi.z))));
        }
        [[nodiscard]] PDFValue pdf(const Vec& wo, const Vec& wi) const {
            if((wo.z * wi.z).val <= 0.0f)
                return { 0.0f };
            const auto wh = halfVector(wo, wi);
            const auto p1 = Dimensionless<float>{ 1.0f } + mDistribution.lambda(wo);
            const auto p2 = mDistribution.lambda(wh);
            return abs(Dimensionless<float>{ 0.25f } / (wo.z * p1 * (p1 + p2)));
        }
        void sample(const float u1, const float u2, const Vec& wo, SurfaceSample& res) const {
            if(wo.z.val == 0.0f)
                return;
            const auto wh = mDistribution.sampleWh(wo, u1, u2);
            if(dot(wo, wh).val < 0.0f)
                return;
            res.wi = reflect(wo, wh);
            if((wo.z * res.wi.z).val <= 0.0f)
                return;

            const auto p1 = Dimensionless<float>{ 1.0f } + mDistribution.lambda(wo);
            res.f = mReflection *
                (mFresnel(dot(res.wi, wh)) *
                 (mDistribution.evaluateD(wh) /
                  (Dimensionless<float>{ 4.0f } * (p1 + mDistribution.lambda(res.wi)) * abs(wo.z * res.wi.z))));
            res.part = part;

            const auto p2 = mDistribution.lambda(wh);
            res.pdf = abs(Dimensionless<float>{ 0.25f } / (wo.z * p1 * (p1 + p2)));
        }
    };

    struct PlasticBSDF final {
        LambertianReflection diffuse;
        MicrofacetReflection<FresnelDielectric> glossy;
        Mask mask;
    };

    extern "C" void PIPER_CC plasticInit(RestrictedContext* context, const void* SBTData, float t, const Vector2<float>& texCoord,
                                         const Normal<float, FOR::Shading>&, const Face face, TransportMode, void* storage,
                                         bool& noSpecular) {
        const auto* data = static_cast<const PlasticData*>(SBTData);
        auto* bsdf = static_cast<PlasticBSDF*>(storage);
        static_assert(sizeof(PlasticBSDF) <= sizeof(SurfaceStorage));
        bsdf->mask = 0;
        Dimensionless<float> diffuse[4];
        piperCall<TextureSampleFunc>(context, data->diffuseTexture, t, texCoord, diffuse);
        const auto diffuseReflection = BxDFValue{ diffuse };
        if(diffuseReflection.valid()) {
            bsdf->mask |= 0b01;
            bsdf->diffuse = LambertianReflection{ diffuseReflection };
        }
        Dimensionless<float> specular[4];
        piperCall<TextureSampleFunc>(context, data->specularTexture, t, texCoord, specular);
        const auto specularReflection = BxDFValue{ specular };
        if(specularReflection.valid()) {
            bsdf->mask |= 0b10;
            Dimensionless<float> roughness;
            piperCall<TextureSampleFunc>(context, data->roughnessTexture, t, texCoord, &roughness);
            roughness = MicrofacetDistribution::remapRoughness(roughness);
            const Dimensionless<float> etaI = { face == Face::Front ? 1.0f : 1.46f };
            const Dimensionless<float> etaT = { face == Face::Front ? 1.46f : 1.0f };
            bsdf->glossy = MicrofacetReflection<FresnelDielectric>{ BxDFValue{ specular }, FresnelDielectric{ etaI, etaT },
                                                                    MicrofacetDistribution{ roughness, roughness } };
        }
        noSpecular = true;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&plasticInit)>);

    KERNEL_FUNCTION_GROUP(plastic, PlasticBSDF, bsdf->diffuse, bsdf->glossy)

    template <typename Fresnel>
    class SpecularReflection final {
    private:
        BxDFValue mReflection;
        Fresnel mFresnel;

    public:
        static constexpr auto part = BxDFPart::Specular | BxDFPart::Reflection;
        explicit SpecularReflection(const BxDFValue& reflection) : mReflection(reflection) {}
        [[nodiscard]] BxDFValue evaluate(const Vec&, const Vec&) const {
            return {};
        }
        [[nodiscard]] PDFValue pdf(const Vec&, const Vec&) const {
            return {};
        }
        [[nodiscard]] SurfaceSample sample(const float, const float, const Vec& wo) const {
            SurfaceSample res;
            res.wi = { { -wo.x, -wo.y, wo.z }, Unsafe{} };
            res.pdf = { 1.0f };
            res.part = part;
            res.f = mFresnel(res.wi.z);
            return res;
        }
    };

    class FresnelSpecular final {
    private:
        BxDFValue mReflection, mTransmission;
        FresnelDielectric mFresnel;
        Dimensionless<float> mEta, mCorrect;

    public:
        static constexpr auto part = BxDFPart::Reflection | BxDFPart::Transmission | BxDFPart::Specular;
        FresnelSpecular(const BxDFValue& reflection, const BxDFValue& transmission, const Dimensionless<float> etaA,
                        const Dimensionless<float> etaB, const TransportMode mode)
            : mReflection(reflection), mTransmission(transmission), mFresnel{ etaA, etaB }, mEta(etaA / etaB),
              mCorrect(mode == TransportMode::Radiance ? square(mEta) : Dimensionless<float>{ 1.0f }) {}

        [[nodiscard]] BxDFValue evaluate(const Vec&, const Vec&) const {
            return {};
        }
        [[nodiscard]] PDFValue pdf(const Vec&, const Vec&) const {
            return {};
        }
        void sample(const float u1, const float u2, const Vec& wo, SurfaceSample& res) const {
            const auto f = mFresnel(wo.z);
            if(u1 < f.val) {
                // specular reflection
                res.wi = { { -wo.x, -wo.y, wo.z }, Unsafe{} };
                res.part = BxDFPart::Reflection | BxDFPart::Specular;
                res.pdf = f;
                res.f = mReflection * (f / abs(res.wi.z));
            } else {
                // specular transmission
                if(!refract(wo, { { { 0.0f }, { 0.0f }, { 1.0f } }, Unsafe{} }, mEta, res.wi))
                    return;

                res.part = BxDFPart::Transmission | BxDFPart::Specular;
                res.pdf = Dimensionless<float>{ 1.0f } - f;
                res.f = mTransmission * (mCorrect * res.pdf / abs(res.wi.z));
            }
        }
    };

    class MicrofacetTransmission final {
    private:
        BxDFValue mTransmission;
        FresnelDielectric mFresnel;
        MicrofacetDistribution mDistribution;
        Dimensionless<float> mEta, mCorrect;

    public:
        static constexpr auto part = BxDFPart::Glossy | BxDFPart::Transmission;
        MicrofacetTransmission(const BxDFValue& transmission, const Dimensionless<float> etaA, const Dimensionless<float> etaB,
                               const MicrofacetDistribution& distribution, const TransportMode mode)
            : mTransmission(transmission), mFresnel{ etaA, etaB }, mDistribution(distribution), mEta(etaB / etaA),
              mCorrect(mode == TransportMode::Radiance ? Dimensionless<float>{ 1.0f } : square(mEta)) {}

        [[nodiscard]] BxDFValue evaluate(const Vec& wo, const Vec& wi) const {
            if((wo.z * wi.z).val >= 0.0f)
                return {};
            auto wh = Vec{ wo.asVector() + wi * mEta };
            if(wh.z.val < 0.0f)
                wh = -wh;

            if((dot(wo, wh) * dot(wi, wh)).val > 0.0f)
                return {};

            const auto f = mFresnel(dot(wo, wh));

            const auto sqrtDenom = dot(wo, wh) + mEta * dot(wi, wh);

            return mTransmission *
                abs((Dimensionless<float>{ 1.0f } - f) * mDistribution.evaluateD(wh) * abs(dot(wi, wh)) * abs(dot(wo, wh)) *
                    mCorrect /
                    ((Dimensionless<float>{ 1.0f } + mDistribution.lambda(wo) + mDistribution.lambda(wi)) * wo.z * wi.z *
                     square(sqrtDenom)));
        }
        [[nodiscard]] PDFValue pdf(const Vec& wo, const Vec& wi) const {
            if((wo.z * wi.z).val >= 0.0f)
                return { 0.0f };
            const auto wh = Vec{ wo.asVector() + wi * mEta };

            if((dot(wo, wh) * dot(wi, wh)).val > 0.0f)
                return { 0.0f };

            const auto sqrtDenom = dot(wo, wh) + mEta * dot(wi, wh);
            const auto delta = abs(square(mEta / sqrtDenom) * dot(wi, wh));
            return mDistribution.evaluateD(wh) * abs(dot(wo, wh)) * delta /
                ((Dimensionless<float>{ 1.0f } + mDistribution.lambda(wo)) * abs(wo.z));
        }
        void sample(const float u1, const float u2, const Vec& wo, SurfaceSample& res) const {
            if(wo.z.val == 0.0f)
                return;
            const auto wh = mDistribution.sampleWh(wo, u1, u2);
            if(dot(wo, wh).val < 0.0f)
                return;

            if(!refract(wo, wh, mEta, res.wi))
                return;
            res.part = part;
            res.f = evaluate(wo, res.wi);
            res.pdf = pdf(wo, res.wi);
        }
    };

    struct GlassBSDF final {
        union {
            FresnelSpecular specular;
            struct {
                MicrofacetReflection<FresnelDielectric> reflection;
                MicrofacetTransmission transmission;
            };
        };
        Mask mask;
    };

    extern "C" void PIPER_CC glassInit(RestrictedContext* context, const void* SBTData, float t, const Vector2<float>& texCoord,
                                       const Normal<float, FOR::Shading>&, Face face, TransportMode mode, void* storage,
                                       bool& noSpecular) {
        const auto* data = static_cast<const GlassData*>(SBTData);
        auto* bsdf = static_cast<GlassBSDF*>(storage);
        static_assert(sizeof(GlassBSDF) <= sizeof(SurfaceStorage));

        Dimensionless<float> reflectionPart[4], transmissionPart[4];
        piperCall<TextureSampleFunc>(context, data->reflection, t, texCoord, reflectionPart);
        piperCall<TextureSampleFunc>(context, data->transmission, t, texCoord, transmissionPart);
        const BxDFValue reflection{ reflectionPart }, transmission{ transmissionPart };

        if(!reflection.valid() && !transmission.valid()) {
            bsdf->mask = 0;
            return;
        }

        Dimensionless<float> roughnessX, roughnessY;
        piperCall<TextureSampleFunc>(context, data->roughnessX, t, texCoord, &roughnessX);
        piperCall<TextureSampleFunc>(context, data->roughnessY, t, texCoord, &roughnessY);

        const Dimensionless<float> etaI = { face == Face::Front ? 1.0f : 1.5f };
        const Dimensionless<float> etaT = { face == Face::Front ? 1.5f : 1.0f };

        if(roughnessX.val == 0.0f && roughnessY.val == 0.0f) {
            bsdf->mask = 0b001;
            noSpecular = false;
            bsdf->specular = FresnelSpecular{ reflection, transmission, etaI, etaT, mode };
        } else {
            bsdf->mask = 0;
            const MicrofacetDistribution distribution{ MicrofacetDistribution::remapRoughness(roughnessX),
                                                       MicrofacetDistribution::remapRoughness(roughnessY) };
            if(reflection.valid()) {
                bsdf->mask |= 0b010;
                bsdf->reflection =
                    MicrofacetReflection<FresnelDielectric>{ reflection, FresnelDielectric{ etaI, etaT }, distribution };
            }
            if(transmission.valid()) {
                bsdf->mask |= 0b100;
                bsdf->transmission = MicrofacetTransmission{ transmission, etaI, etaT, distribution, mode };
            }
            noSpecular = true;
        }
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&glassInit)>);

    KERNEL_FUNCTION_GROUP(glass, GlassBSDF, bsdf->specular, bsdf->reflection, bsdf->transmission)

    // TODO:Metal

    struct MirrorBSDF final {
        BxDFValue reflection;
    };

    extern "C" void PIPER_CC mirrorInit(RestrictedContext* context, const void* SBTData, float t, const Vector2<float>& texCoord,
                                        const Normal<float, FOR::Shading>&, Face, TransportMode, void* storage,
                                        bool& noSpecular) {
        const auto* data = static_cast<const MirrorData*>(SBTData);
        auto* bsdf = static_cast<MirrorBSDF*>(storage);
        static_assert(sizeof(MirrorBSDF) <= sizeof(SurfaceStorage));
        Dimensionless<float> reflection[4];
        piperCall<TextureSampleFunc>(context, data->reflectionTexture, t, texCoord, reflection);
        bsdf->reflection = BxDFValue{ reflection };
        noSpecular = false;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&mirrorInit)>);

    extern "C" void PIPER_CC mirrorSample(RestrictedContext*, const void*, const void* storage, const Vec& wo,
                                          const Normal<float, FOR::Shading>&, const BxDFPart require, SurfaceSample& sample) {
        const auto* bsdf = static_cast<const MirrorBSDF*>(storage);
        constexpr auto part = BxDFPart::Reflection | BxDFPart::Specular;
        if(match(part, require)) {
            sample.wi = { { -wo.x, -wo.y, wo.z }, Unsafe{} };
            sample.f = bsdf->reflection / abs(sample.wi.z);
            sample.part = part;
            sample.pdf = { 1.0f };
        }
    }
    static_assert(std::is_same_v<SurfaceSampleFunc, decltype(&mirrorSample)>);

    class FresnelBlend final {
    private:
        BxDFValue mDiffuse, mSpecular;
        MicrofacetDistribution mDistribution;

    public:
        FresnelBlend(const BxDFValue& diffuse, const BxDFValue& specular, const MicrofacetDistribution& distribution)
            : mDiffuse(diffuse), mSpecular(specular), mDistribution(distribution) {}

        static constexpr auto part = BxDFPart::Glossy | BxDFPart::Reflection;

        [[nodiscard]] BxDFValue evaluate(const Vec& wo, const Vec& wi) const {
            const auto pow5 = [](const Dimensionless<float> v) { return (v * v) * (v * v) * v; };
            const auto negRemSpecular = mSpecular + BxDFValue{ { -1.0f }, { -1.0f }, { -1.0f } };
            const auto diffuse = mDiffuse * negRemSpecular *
                (Dimensionless<float>{ -28.f / (23.f * Constants::pi<float>)} *
                 (Dimensionless<float>{ 1.0f } - pow5({ 1.0f - 0.5f * std::fabs(wi.z.val) })) *
                 (Dimensionless<float>{ 1.0f } - pow5({ 1.0f - 0.5f * std::fabs(wo.z.val) })));
            const auto wh = halfVector(wo, wi);

            auto schlickFresnel = [&, this](const Dimensionless<float> cosTheta) {
                return mSpecular + negRemSpecular * pow5(cosTheta - Dimensionless<float>{ 1.0f });
            };
            const auto specular = schlickFresnel(dot(wi, wh)) *
                (mDistribution.evaluateD(wh) /
                 (Dimensionless<float>{ 4.0f * std::max(std::fabs(wi.z.val), std::fabs(wo.z.val)) } * abs(dot(wi, wh))));
            return diffuse + specular;
        }
        [[nodiscard]] PDFValue pdf(const Vec& wo, const Vec& wi) const {
            if((wo.z * wi.z).val <= 0.0f)
                return { 0.0f };
            const auto wh = halfVector(wo, wi);
            return Dimensionless<float>{ 0.5f } *
                (abs(wi.z) * Dimensionless<float>{ Constants::invPi<float> } +
                 mDistribution.evaluateD(wh) /
                     (Dimensionless<float>{ 4.0f } * abs(wo.z) *
                      (Dimensionless<float>{ 1.0f } + mDistribution.lambda(wo) + mDistribution.lambda(wh))));
        }
        void sample(float u1, const float u2, const Vec& wo, SurfaceSample& res) const {
            if(u1 < 0.5f) {
                u1 = std::fmin(2.0f * u1, 1.0f);
                res.wi = sampleCosineHemisphere(u1, u2);
            } else {
                u1 = std::fmin(2.0f * (u1 - 0.5f), 1.0f);
                const auto wh = mDistribution.sampleWh(wo, u1, u2);
                res.wi = reflect(wo, wh);
                if((wo.z * res.wi.z).val <= 0.0f)
                    return;
            }
            res.part = part;
            res.pdf = pdf(wo, res.wi);
            res.f = evaluate(wo, res.wi);
        }
    };

    struct SubstrateBSDF final {
        FresnelBlend blend;
        Mask mask;
    };

    extern "C" void PIPER_CC substrateInit(RestrictedContext* context, const void* SBTData, float t,
                                           const Vector2<float>& texCoord, const Normal<float, FOR::Shading>&, Face face,
                                           TransportMode mode, void* storage, bool& noSpecular) {
        const auto* data = static_cast<const SubstrateData*>(SBTData);
        auto* bsdf = static_cast<SubstrateBSDF*>(storage);
        static_assert(sizeof(SubstrateBSDF) <= sizeof(SurfaceStorage));

        Dimensionless<float> diffusePart[4], specularPart[4];
        piperCall<TextureSampleFunc>(context, data->diffuse, t, texCoord, diffusePart);
        piperCall<TextureSampleFunc>(context, data->specular, t, texCoord, specularPart);
        const BxDFValue diffuse{ diffusePart }, specular{ specularPart };

        if(!diffuse.valid() && !specular.valid())
            return;

        Dimensionless<float> roughnessX, roughnessY;
        piperCall<TextureSampleFunc>(context, data->roughnessX, t, texCoord, &roughnessX);
        piperCall<TextureSampleFunc>(context, data->roughnessY, t, texCoord, &roughnessY);
        const MicrofacetDistribution distribution{ MicrofacetDistribution::remapRoughness(roughnessX),
                                                   MicrofacetDistribution::remapRoughness(roughnessY) };

        bsdf->mask = 1;
        bsdf->blend = FresnelBlend{ diffuse, specular, distribution };
        noSpecular = true;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&substrateInit)>);

    KERNEL_FUNCTION_GROUP(substrate, SubstrateBSDF, bsdf->blend)

    // TODO:Translucent
}  // namespace Piper
