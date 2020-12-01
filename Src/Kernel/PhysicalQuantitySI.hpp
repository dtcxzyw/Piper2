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
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace Piper {
    // https://www.bipm.org/en/measurement-units/

    template <typename Float, int32_t m, int32_t kg, int32_t s, int32_t A, int32_t K, int32_t mol, int32_t cd, int32_t rad,
              int32_t sr>
    struct PhysicalQuantitySI final {
        using FT = Float;
        static constexpr auto vm = m, vkg = kg, vs = s, vA = A, vK = K, vmol = mol, vcd = cd, vrad = rad, vsr = sr;
        Float val;

        PhysicalQuantitySI operator-() const noexcept {
            return { -val };
        }
        PhysicalQuantitySI operator+(const PhysicalQuantitySI rhs) const noexcept {
            return { val + rhs.val };
        }
        PhysicalQuantitySI operator-(const PhysicalQuantitySI rhs) const noexcept {
            return { val - rhs.val };
        }
    };

    template <typename T1, typename T2, typename = std::enable_if_t<std::is_same_v<typename T1::FT, typename T2::FT>>>
    struct ProductImpl final {
        using Type =
            PhysicalQuantitySI<typename T1::FT, T1::vm + T2::vm, T1::vkg + T2::vkg, T1::vs + T2::vs, T1::vA + T2::vA,
                               T1::vK + T2::vK, T1::vmol + T2::vmol, T1::vcd + T2::vcd, T1::vrad + T2::vrad, T1::vsr + T2::vsr>;
    };

    template <typename T1, typename T2>
    using Product = typename ProductImpl<T1, T2>::Type;

    template <typename T1, typename T2, typename = std::enable_if_t<std::is_same_v<typename T1::FT, typename T2::FT>>>
    struct RatioImpl final {
        using Type =
            PhysicalQuantitySI<typename T1::FT, T1::vm - T2::vm, T1::vkg - T2::vkg, T1::vs - T2::vs, T1::vA - T2::vA,
                               T1::vK - T2::vK, T1::vmol - T2::vmol, T1::vcd - T2::vcd, T1::vrad - T2::vrad, T1::vsr - T2::vsr>;
    };

    template <typename T1, typename T2>
    using Ratio = typename RatioImpl<T1, T2>::Type;

    template <typename T>
    struct SquareImpl final {
        using Type = PhysicalQuantitySI<typename T::FT, 2 * T::vm, 2 * T::vkg, 2 * T::vs, 2 * T::vA, 2 * T::vK, 2 * T::vmol,
                                        2 * T::vcd, 2 * T::vrad, 2 * T::vsr>;
    };

    template <typename T>
    using Square = typename SquareImpl<T>::Type;

    template <typename T>
    struct InverseImpl final {
        using Type =
            PhysicalQuantitySI<typename T::FT, -T::vm, -T::vkg, -T::vs, -T::vA, -T::vK, -T::vmol, -T::vcd, -T::vrad, -T::vsr>;
    };

    template <typename T>
    using Inverse = typename InverseImpl<T>::Type;
    template <typename T>
    constexpr auto inverse(T x) {
        return Inverse<T>{ static_cast<typename T::FT>(1.0) / x.val };
    }

    namespace Detail {
        template <typename... T>
        constexpr bool isEven(T... args) {
            return (... && static_cast<bool>(args % 2 == 0));
        }
    }  // namespace detail

    template <typename T,
              typename = std::enable_if_t<Detail::isEven(T::vm, T::vkg, T::vs, T::vA, T::vK, T::vmol, T::vcd, T::vrad, T::vsr)>>
    struct SqrtImpl final {
        using Type = PhysicalQuantitySI<typename T::FT, T::vm / 2, T::vkg / 2, T::vs / 2, T::vA / 2, T::vK / 2, T::vmol / 2,
                                        T::vcd / 2, T::vrad / 2, T::vsr / 2>;
    };

    template <typename T>
    using Sqrt = typename SqrtImpl<T>::Type;

    template <typename T, typename = Sqrt<T>>
    auto sqrt(T val) noexcept {
        return Sqrt<T>{ std::sqrt(val.val) };
    }

    template <typename T1, typename T2, typename = Product<T1, T2>>
    constexpr auto operator*(T1 a, T2 b) noexcept {
        return Product<T1, T2>{ a.val * b.val };
    }
    template <typename T1, typename T2, typename = Ratio<T1, T2>>
    constexpr auto operator/(T1 a, T2 b) noexcept {
        return Ratio<T1, T2>{ a.val / b.val };
    }
    template <typename T>
    constexpr auto dot(T a, T b) noexcept {
        return T{ a.val * b.val };
    }

    template <typename Float>
    using Dimensionless = PhysicalQuantitySI<Float, 0, 0, 0, 0, 0, 0, 0, 0, 0>;

    template <typename T>
    constexpr auto eraseUnit(T val) noexcept {
        return Dimensionless<typename T::FT>{ val.val };
    }

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

    template <typename T>
    auto cos(Radian<T> x) {
        return Dimensionless<T>{ std::cos(x.val) };
    }

    template <typename T>
    auto sin(Radian<T> x) {
        return Dimensionless<T>{ std::sin(x.val) };
    }

    template <typename Float>
    using SolidAngle = PhysicalQuantitySI<Float, 0, 0, 0, 0, 0, 0, 0, 0, 1>;

    template <typename Float>
    using Frequency = Inverse<Time<Float>>;

    template <typename Float>
    using Area = Product<Length<Float>, Length<Float>>;

    template <typename Float>
    using Volume = Product<Area<Float>, Length<Float>>;

    template <typename Float>
    using Velocity = Ratio<Length<Float>, Time<Float>>;

    template <typename Float>
    using Acceleration = Ratio<Velocity<Float>, Time<Float>>;

    template <typename Float>
    using Force = Product<Mass<Float>, Acceleration<Float>>;

    template <typename Float>
    using Energy = Product<Force<Float>, Length<Float>>;

    template <typename Float>
    using Work = Energy<Float>;

    template <typename Float>
    using Charge = Product<ElectricCurrent<Float>, Time<Float>>;

    template <typename Float>
    using LuminousFlux = Product<LuminousIntensity<Float>, SolidAngle<Float>>;

    template <typename Float>
    using Power = Ratio<Work<Float>, Time<Float>>;

    namespace Constants {
        template <typename Float>
        constexpr Frequency<Float> vCs = static_cast<Float>(9'192'631'770);

        template <typename Float>
        constexpr Velocity<Float> c = static_cast<Float>(299'792'458);

        template <typename Float>
        constexpr typename Product<Energy<Float>, Time<Float>>::Type h = static_cast<Float>(6.62607015e-34);

        template <typename Float>
        constexpr Charge<Float> e = static_cast<Float>(1.602176634e-19);

        template <typename Float>
        constexpr Ratio<Energy<Float>, Temperature<Float>> k = static_cast<Float>(1.380649e-23);

        template <typename Float>
        constexpr Inverse<AmountOfSubstance<Float>> Na = static_cast<Float>(6.02214076e23);

        template <typename Float>
        constexpr Ratio<LuminousFlux<Float>, Power<Float>> Kcd = static_cast<Float>(683);

        template <typename Float>
        constexpr Radian<Float> pi = Radian<Float>{ static_cast<Float>(3.1415926535897932384626433832795) };

        template <typename Float>
        constexpr SolidAngle<Float> areaOfSphere = SolidAngle<Float>{ static_cast<Float>(12.566370614359172953850573533118) };

    }  // namespace Constants

}  // namespace Piper
