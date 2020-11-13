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
#include <cstdint>

namespace Piper {
    // https://www.bipm.org/en/measurement-units/

    template <typename Float, int32_t m, int32_t kg, int32_t s, int32_t A, int32_t K, int32_t mol, int32_t cd, int32_t rad, int32_t sr>
    struct PhysicalQuantitySI final {
        Float val;
    };

    template <template <typename Float, int32_t m1, int32_t kg1, int32_t s1, int32_t A1, int32_t K1, int32_t mol1, int32_t cd1, int32_t rad1, int32_t sr1> class T1,
              template <typename Float, int32_t m2, int32_t kg2, int32_t s2, int32_t A2, int32_t K2, int32_t mol2, int32_t cd2, int32_t rad2, int32_t sr2> class T2>
    using Product =
        PhysicalQuantitySI<Float, m1 + m2, kg1 + kg2, s1 + s2, A1 + A2, K1 + K2, mol1 + mol2, cd1 + cd2, rad1 + rad2, sr1 + sr2>;

    template <template <typename Float, int32_t m1, int32_t kg1, int32_t s1, int32_t A1, int32_t K1, int32_t mol1, int32_t cd1, int32_t rad1, int32_t sr1> class T1,
              template <typename Float, int32_t m2, int32_t kg2, int32_t s2, int32_t A2, int32_t K2, int32_t mol2, int32_t cd2, int32_t rad2, int32_t sr2> class T2>
    using Ratio =
        PhysicalQuantitySI<Float, m1 - m2, kg1 - kg2, s1 - s2, A1 - A2, K1 - K2, mol1 - mol2, cd1 - cd2, rad1 - rad2, sr1 - sr2>;

    template <template <typename Float, int32_t m, int32_t kg, int32_t s, int32_t A, int32_t K, int32_t mol, int32_t cd, int32_t rad, int32_t sr> class T>
    using Square = PhysicalQuantitySI<Float, 2 * m, 2 * kg, 2 * s, 2 * A, 2 * K, 2 * mol, 2 * cd, 2 * rad, 2 * sr>;

    template <template <typename Float, int32_t m, int32_t kg, int32_t s, int32_t A, int32_t K, int32_t mol, int32_t cd, int32_t rad, int32_t sr> class T>
    using Inverse = PhysicalQuantitySI<Float, -m, -kg, -s, -A, -K, -mol, -cd, -rad, -sr>;

    template <typename Float>
    using Length = PhysicalQuantitySI<Float, 1, 0, 0, 0, 0, 0, 0, 0, 0>;

    template <typename Float>
    using Mass = PhysicalQuantitySI<Float, 0, 1, 0, 0, 0, 0, 0, 0, 0>;

    template <typename Float>
    using Time = PhysicalQuantitySI<Float, 0, 0, 1, 0, 0, 0, 0, 0, 0>;

    template <typename Float>
    using ElectricCurrent = PhysicalQuantitySI<Float, 0, 0, 0, 1, 0, 0, 0, 0, 0>;

    template <typename Float>
    using Temperature = PhysicalQuantitySI<Float, 0, 0, 0, 0, 1, 0, 0, 0, 0>;

    template <typename Float>
    using AmountOfSubstance = PhysicalQuantitySI<Float, 0, 0, 0, 0, 0, 1, 0, 0, 0>;

    template <typename Float>
    using LuminousIntensity = PhysicalQuantitySI<Float, 0, 0, 0, 0, 0, 0, 1, 0, 0>;

    template <typename Float>
    using Radian = PhysicalQuantitySI<Float, 0, 0, 0, 0, 0, 0, 0, 1, 0>;

    template <typename Float>
    using Steradian = PhysicalQuantitySI<Float, 0, 0, 0, 0, 0, 0, 0, 0, 1>;

    template <typename Float>
    using Frequency = Inverse<Time<Float>>;

    template <typename Float>
    using Velocity = Ratio<Length<Float>, Time<Float>>;

    template <typename Float>
    using Acceleration = Ratio<Velocity<Float>, Time<Float>>;

    template <typename Float>
    using Force = Product<Mass<Float>, Time<Float>>;

    template <typename Float>
    using Energy = Product<Force<Float>, Length<Float>>;

    template <typename Float>
    using Work = Energy<Float>;

    template <typename Float>
    using Charge = Product<ElectricCurrent<Float>, Time<Float>>;

    template <typename Float>
    using LuminousFlux = Product<LuminousIntensity<Float>, Steradian<Float>>;

    template <typename Float>
    using Power = Ratio<Work<Float>, Time<Float>>;

    namespace Constants {
        template <typename Float>
        constexpr Frequency<Float> vCs = static_cast<Float>(9'192'631'770);

        template <typename Float>
        constexpr Velocity<Float> c = static_cast<Float>(299'792'458);

        template <typename Float>
        constexpr Product<Energy<Float>, Time<Float>> h = static_cast<Float>(6.62607015e–34);

        template <typename Float>
        constexpr Charge<Float> e = static_cast<Float>(1.602176634e-19);

        template <typename Float>
        constexpr Ratio<Energy<Float>, Temperature<Float>> k = static_cast<Float>(1.380649e–23);

        template <typename Float>
        constexpr Inverse<AmountOfSubstance<Float>> Na = static_cast<Float>(6.02214076e23);

        template <typename Float>
        constexpr Ratio<LuminousFlux<Float>, Power<Float>> Kcd = static_cast<Float>(683);

        template <typename Float>
        constexpr Radian<Float> pi = static_cast<Float>(3.1415926535897932384626433832795);

        template <typename Float>
        constexpr Steradian<Float> areaOfSphere = static_cast<Float>(12.566370614359172953850573533118);

    }  // namespace Constants

}  // namespace Piper
