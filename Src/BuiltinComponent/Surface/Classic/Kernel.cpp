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
                                           const Normal<float, FOR::Shading>&, void*, bool& noSpecular) {
        noSpecular = false;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&blackBodyInit)>);
    extern "C" void PIPER_CC blackBodySample(RestrictedContext*, const void*, const void*, const Vec&,
                                             const Normal<float, FOR::Shading>&, BxDFPart, SurfaceSample&) {}
    static_assert(std::is_same_v<SurfaceSampleFunc, decltype(&blackBodySample)>);
    extern "C" void PIPER_CC blackBodyEvaluate(RestrictedContext*, const void*, const void*, const Vec&, const Vec&,
                                               const Normal<float, FOR::Shading>&, BxDFPart, BxDFValue& f) {
        f = { { 0.0f }, { 0.0f }, { 0.0f } };
    }
    static_assert(std::is_same_v<SurfaceEvaluateFunc, decltype(&blackBodyEvaluate)>);
    extern "C" void PIPER_CC blackBodyPdf(RestrictedContext*, const void*, const void*, const Vec&, const Vec&,
                                          const Normal<float, FOR::Shading>&, BxDFPart, Dimensionless<float>& pdf) {
        pdf = Dimensionless<float>{ 0.0f };
    }
    static_assert(std::is_same_v<SurfacePdfFunc, decltype(&blackBodyPdf)>);

    template <typename T>
    struct DefaultBxDFHelper {
        void sample(const float u1, const float u2, const Vec& wo, BxDFPart, SurfaceSample& sample) const {
            sample.wi = sampleCosineHemisphere(u1, u2);
            auto&& self = *static_cast<const T*>(this);
            self.evaluate(wo, sample.wi, sample.f);
            pdf(wo, sample.wi, sample.pdf);
            sample.part = T::part;
        }
        void pdf(const Vec& wo, const Vec& wi, PDFValue& pdf) const {
            pdf = ((wo.z * wi.z).val > 0.0f ? PDFValue{ std::abs(wi.z.val) * Constants::invPi<float> } : PDFValue{ 0.0f });
        }
    };

    class LambertianReflection final : public DefaultBxDFHelper<LambertianReflection> {
    private:
        BxDFValue mDiffuse;

    public:
        static constexpr BxDFPart part = BxDFPart::Diffuse | BxDFPart::Reflection;
        explicit LambertianReflection(const BxDFValue& diffuse) : mDiffuse(diffuse) {}
        void evaluate(const Vec&, const Vec&, BxDFValue& f) const {
            f = mDiffuse * Dimensionless<float>{ Constants::invPi<float> };
        }
    };

    void sampleN(uint32_t i, float u1, float u2, const Vec& wo, BxDFPart require, SurfaceSample& sample) {
        // TODO:assertion failed
    }
    template <typename First, typename... BxDFs>
    void sampleN(uint32_t i, float u1, float u2, const Vec& wo, BxDFPart require, SurfaceSample& sample, const First& current,
                 const BxDFs&... bxdfs) {
        auto flag = match(First::part, require);
        if(flag & (i == 0))
            current.sample(u1, u2, wo, require, sample);
        else
            sampleN(i - flag, u1, u2, wo, require, sample, bxdfs...);
    }
    void pdfN(uint32_t, const Vec&, const Vec&, BxDFPart, PDFValue&) {}
    template <typename First, typename... BxDFs>
    void pdfN(uint32_t i, const Vec& wo, const Vec& wi, BxDFPart require, PDFValue& pdf, const First& current,
              const BxDFs&... bxdfs) {
        auto flag = match(First::part, require);
        if(flag & (i != 0)) {
            Dimensionless<float> sub;
            current.pdf(wo, wi, sub);
            pdf = pdf + sub;
        }
        pdfN(i - flag, wo, wi, require, pdf, bxdfs...);
    }
    void evaluateN(uint32_t, const Vec&, const Vec&, BxDFPart, BxDFPart, BxDFValue&) {}
    template <typename First, typename... BxDFs>
    void evaluateN(uint32_t i, const Vec& wo, const Vec& wi, BxDFPart require, BxDFPart addition, BxDFValue& f,
                   const First& current, const BxDFs&... bxdfs) {
        auto flag = match(First::part, require);
        if(flag & (i != 0) & (addition & First::part)) {
            BxDFValue sub;
            current.evaluate(wo, wi, sub);
            f = f + sub;
        }
        evaluateN(i - flag, wo, wi, require, addition, f, bxdfs...);
    }

    template <typename... BxDFs>
    void sample(float u1, float u2, const Vec& wo, const Normal<float, FOR::Shading>& Ng, BxDFPart require, SurfaceSample& sample,
                const BxDFs&... bxdfs) {
        const auto count = (static_cast<uint32_t>(match(BxDFs::part, require)) + ...);
        if(count == 0) {
            sample.part = static_cast<BxDFPart>(0);
            return;
        }
        const auto select = std::min(static_cast<uint32_t>(std::floor(u1 * static_cast<float>(count))), count - 1);
        u1 = u1 * static_cast<float>(count) - static_cast<float>(select);
        sampleN(select, u1, u2, wo, require, sample, bxdfs...);
        if(static_cast<uint32_t>(sample.part) == 0)
            return;
        if(count > 1 && !(sample.part & BxDFPart::Specular)) {
            const auto addition = ((dot(wo, Ng) * dot(sample.wi, Ng)).val > 0.0f ? BxDFPart::Reflection : BxDFPart::Refraction);
            evaluateN(select, wo, sample.wi, require, addition, sample.f, bxdfs...);
            pdfN(select, wo, sample.wi, require, sample.pdf, bxdfs...);
        }
        if(count > 1)
            sample.pdf = sample.pdf * Dimensionless<float>{ 1.0f / count };
    }

    struct MatteBSDF final {
        LambertianReflection lambertian;
    };

    extern "C" void PIPER_CC matteInit(RestrictedContext*, const void* SBTData, float, const Vector2<float>&,
                                       const Normal<float, FOR::Shading>&, void* storage, bool& noSpecular) {
        const auto* data = static_cast<const MatteData*>(SBTData);
        auto* bsdf = static_cast<MatteBSDF*>(storage);
        static_assert(sizeof(MatteBSDF) <= sizeof(SurfaceStorage));
        bsdf->lambertian = LambertianReflection{ data->diffuse };
        noSpecular = true;
    }
    static_assert(std::is_same_v<SurfaceInitFunc, decltype(&matteInit)>);

    extern "C" void PIPER_CC matteSample(RestrictedContext* context, const void*, const void* storage, const Vec& wo,
                                         const Normal<float, FOR::Shading>& Ng, BxDFPart require, SurfaceSample& sample) {
        const auto* bsdf = static_cast<const MatteBSDF*>(storage);
        Piper::sample(piperSample(context), piperSample(context), wo, Ng, require, sample, bsdf->lambertian);
    }
    static_assert(std::is_same_v<SurfaceSampleFunc, decltype(&matteSample)>);

    extern "C" void PIPER_CC matteEvaluate(RestrictedContext* context, const void*, const void* storage, const Vec& wo,
                                           const Vec& wi, const Normal<float, FOR::Shading>& Ng, BxDFPart require, BxDFValue& f) {
        const auto* bsdf = static_cast<const MatteBSDF*>(storage);
        const auto addition = ((dot(wo, Ng) * dot(wi, Ng)).val > 0.0f ? BxDFPart::Reflection : BxDFPart::Refraction);
        f = {};
        evaluateN(std::numeric_limits<uint32_t>::max(), wo, wi, require, addition, f, bsdf->lambertian);
    }
    static_assert(std::is_same_v<SurfaceEvaluateFunc, decltype(&matteEvaluate)>);

    extern "C" void PIPER_CC mattePdf(RestrictedContext*, const void*, const void* storage, const Vec& wo, const Vec& wi,
                                      const Normal<float, FOR::Shading>& Ng, BxDFPart require, PDFValue& pdf) {
        const auto* bsdf = static_cast<const MatteBSDF*>(storage);
        pdf = {};
        pdfN(std::numeric_limits<uint32_t>::max(), wo, wi, require, pdf, bsdf->lambertian);
    }
    static_assert(std::is_same_v<SurfacePdfFunc, decltype(&mattePdf)>);
}  // namespace Piper
