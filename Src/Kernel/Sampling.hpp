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

namespace Piper {

    template <typename T>
    Vector2<Dimensionless<T>> sampleUniformDisk(T u1, T u2) {
        auto ang = u1 * Constants::twoPi<T>;
        auto rad = std::sqrt(u2);
        // TODO:use sincos?
        return { { rad * std::cos(ang) }, { rad * std::sin(ang) } };
    }

    template <typename T>
    Vector2<Dimensionless<T>> sampleConcentricDisk(T u1, T u2) {
        u1 = static_cast<T>(2) * u1 - static_cast<T>(1);
        u2 = static_cast<T>(2) * u2 - static_cast<T>(1);
        auto au1 = std::fabs(u1), au2 = std::fabs(u2);
        if(std::fmin(au1, au2) < static_cast<T>(1e-4))
            return Vector2<Dimensionless<T>>{ static_cast<T>(0), static_cast<T>(0) };
        auto rad = (au1 > au2 ? u1 : u2);
        auto theta =
            (au1 > au2 ? Constants::quarterPi<T> * (u2 / u1) : Constants::halfPi<T> - Constants::quarterPi<T> * (u1 / u2));
        return Vector2<Dimensionless<T>>{ std::cos(theta), std::sin(theta) } * Dimensionless<T>{ rad };
    }

    template <typename T>
    Normal<T, FOR::Shading> sampleCosineHemisphere(T u1, T u2) {
        auto coord = sampleConcentricDisk(u1, u2);
        auto z = std::sqrt(std::fmax(static_cast<T>(0), static_cast<T>(1) - lengthSquared(coord).val));
        return Normal<T, FOR::Shading>{ Vector<Dimensionless<T>, FOR::Shading>{ coord.x, coord.y, Dimensionless<T>{ z } },
                                        Unchecked{} };
    }
}  // namespace Piper
