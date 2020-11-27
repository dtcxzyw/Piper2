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
    extern "C" void PIPER_CC blackBodySample(RestrictedContext* context, const void* SBTData,
                                             const Normal<float, FOR::Shading>& wi, float t, SurfaceSample& sample) {
        sample.type = RayType::Reflection;
        sample.wo = wi;
        sample.f = { { 0.0f }, { 0.0f }, { 0.0f } };
    }
    static_assert(std::is_same_v<SurfaceSampleFunc, decltype(&blackBodySample)>);
    extern "C" void PIPER_CC blackBodyEvaluate(RestrictedContext* context, const void* SBTData,
                                               const Normal<float, FOR::Shading>& wi, const Normal<float, FOR::Shading>& wo,
                                               float t, Spectrum<Dimensionless<float>>& f) {
        f = { { 0.0f }, { 0.0f }, { 0.0f } };
    }
    static_assert(std::is_same_v<SurfaceEvaluateFunc, decltype(&blackBodyEvaluate)>);
}  // namespace Piper
