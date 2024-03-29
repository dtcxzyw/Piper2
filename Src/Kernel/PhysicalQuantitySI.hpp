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
    constexpr auto square(T x) {
        return Square<T>{ x.val * x.val };
    }

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
    }  // namespace Detail

    template <typename T,
              typename = std::enable_if_t<Detail::isEven(T::vm, T::vkg, T::vs, T::vA, T::vK, T::vmol, T::vcd, T::vrad, T::vsr)>>
    struct SqrtImpl final {
        using Type = PhysicalQuantitySI<typename T::FT, T::vm / 2, T::vkg / 2, T::vs / 2, T::vA / 2, T::vK / 2, T::vmol / 2,
                                        T::vcd / 2, T::vrad / 2, T::vsr / 2>;
    };

    template <typename T>
    using Sqrt = typename SqrtImpl<T>::Type;

    template <typename T, typename = Sqrt<T>>
    auto sqrtSafe(T val) noexcept {
        return Sqrt<T>{ std::sqrt(std::fmax(val.val, static_cast<typename T::FT>(0.0))) };
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
    constexpr auto abs(Dimensionless<T> val) {
        return Dimensionless<T>{ std::fabs(val.val) };
    }

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

    template <typename T>
    constexpr auto acosSafe(Dimensionless<T> a) noexcept {
        return Radian<T>{ std::acos(std::fmax(static_cast<T>(-1.0), std::fmin(static_cast<T>(1.0), a.val))) };
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
        constexpr Float pi = static_cast<Float>(3.14159265358979323846);

        template <typename Float>
        constexpr Float twoPi = static_cast<Float>(6.2831853071796);

        template <typename Float>
        constexpr Float areaOfSphere = static_cast<Float>(12.566370614359172953850573533118);

        template <typename Float>
        constexpr Float halfPi = static_cast<Float>(1.57079632679489661923);

        template <typename Float>
        constexpr Float quarterPi = static_cast<Float>(0.785398163397448309616);

        template <typename Float>
        constexpr Float invPi = static_cast<Float>(0.318309886183790671538);

        template <typename Float>
        constexpr Float twoInvPi = static_cast<Float>(0.636619772367581343076);

        template <typename Float>
        constexpr Float sqrPi = static_cast<Float>(9.869604401089358);

    }  // namespace Constants

    template <typename T>
    constexpr auto atan2(Dimensionless<T> y, Dimensionless<T> x) noexcept {
        const auto theta = std::atan2(y.val, x.val);
        return Radian<T>{ theta < static_cast<T>(0.0) ? theta + Constants::twoPi<T> : theta };
    }
}  // namespace Piper
