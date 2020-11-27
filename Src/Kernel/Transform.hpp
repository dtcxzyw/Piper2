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

namespace Piper {
    // TODO:unit test
    // TODO:explicit NotInitialized
    // struct NotInitialized final {};
    struct Unchecked final {};

    template <typename Float>
    struct Vector2 {
        Float x, y;
        Vector2 operator+(Vector2 rhs) const noexcept {
            return Vector2{ x + rhs.x, y + rhs.y };
        }
        Vector2& operator+=(Vector2 rhs) noexcept {
            x += rhs.x;
            y += rhs.y;
            return *this;
        }

        Vector2 operator-(Vector2 rhs) const noexcept {
            return Vector2{ x - rhs.x, y - rhs.y };
        }
        Vector2& operator-=(Vector2 rhs) noexcept {
            x -= rhs.x;
            y -= rhs.y;
            return *this;
        }

        Vector2 operator*(Vector2 rhs) const noexcept {
            return Vector2{ x * rhs.x, y * rhs.y };
        }
        Vector2& operator*=(Vector2 rhs) noexcept {
            x *= rhs.x;
            y *= rhs.y;
            return *this;
        }

        Vector2 operator/(Vector2 rhs) const noexcept {
            return Vector2{ x / rhs.x, y / rhs.y };
        }
        Vector2& operator/=(Vector2 rhs) noexcept {
            x /= rhs.x;
            y /= rhs.y;
            return *this;
        }

        Vector2 operator*(Float rhs) const noexcept {
            return Vector2{ x * rhs, y * rhs };
        }
        Vector2& operator*=(Float rhs) noexcept {
            x *= rhs;
            y *= rhs;
            return *this;
        }

        Vector2 operator/(Float rhs) const noexcept {
            return Vector2{ x / rhs, y / rhs };
        }
        Vector2& operator/=(Float rhs) noexcept {
            x /= rhs;
            y /= rhs;
            return *this;
        }
        Vector2 operator-() const noexcept {
            return { -x, -y };
        }
    };

    template <typename Float>
    Vector2<Float> operator*(Float lhs, Vector2<Float> rhs) noexcept {
        return rhs * lhs;
    }

    template <typename Float>
    Float dot(Vector2<Float> a, Vector2<Float> b) noexcept {
        return a.x * b.x + a.y * b.y;
    }

    template <typename Float>
    Float cross(Vector2<Float> a, Vector2<Float> b) noexcept {
        return a.x * b.y - a.y * b.x;
    }

    template <typename Float>
    Float lengthSquared(Vector2<Float> a) noexcept {
        return a.x * a.x + a.y * a.y;
    }

    template <typename Float>
    Float length(Vector2<Float> a) noexcept {
        return std::hypot(a.x, a.y);
    }

    template <typename Float>
    Float distanceSquared(Vector2<Float> a, Vector2<Float> b) noexcept {
        return lengthSquared(a - b);
    }

    template <typename Float>
    Float distance(Vector2<Float> a, Vector2<Float> b) noexcept {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    template <typename Float>
    Vector2<Float> normalize(Vector2<Float> a) noexcept {
        return a / length(a);
    }

    template <typename Float>
    Vector2<Float> lerp(Vector2<Float> a, Vector2<Float> b, Float u) noexcept {
        return a * (static_cast<Float>(1.0) - u) + b * u;
    }

    enum class FOR { World, Local, Shading };

    template <typename Float, FOR ref>
    struct Vector {
        Float x, y, z;
        Vector operator+(Vector rhs) const noexcept {
            return Vector{ x + rhs.x, y + rhs.y, z + rhs.z };
        }
        Vector& operator+=(Vector rhs) noexcept {
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
            return *this;
        }

        Vector operator-(Vector rhs) const noexcept {
            return Vector{ x - rhs.x, y - rhs.y, z - rhs.z };
        }
        Vector& operator-=(Vector rhs) noexcept {
            x -= rhs.x;
            y -= rhs.y;
            z -= rhs.z;
            return *this;
        }

        template <typename U>
        auto operator*(U rhs) const noexcept {
            using ResultT = decltype(x * rhs);
            return Vector<ResultT, ref>{ x * rhs, y * rhs, z * rhs };
        }
        template <typename U>
        auto operator/(U rhs) const noexcept {
            using ResultT = decltype(x / rhs);
            return Vector<ResultT, ref>{ x / rhs, y / rhs, z / rhs };
        }
        Vector operator-() const noexcept {
            return { -x, -y, -z };
        }
    };

    template <typename T, typename U, FOR ref>
    auto operator*(T lhs, Vector<U, ref> rhs) noexcept {
        return rhs * lhs;
    }

    template <typename Float, FOR ref>
    auto lengthSquared(Vector<Float, ref> a) noexcept {
        return a.x * a.x + a.y * a.y + a.z * a.z;
    }

    template <typename Float, FOR ref>
    Float length(Vector<Float, ref> a) noexcept {
        return sqrt(lengthSquared(a));
    }

    template <typename Float, FOR ref>
    Vector<Float, ref> lerp(Vector<Float, ref> a, Vector<Float, ref> b, Float u) noexcept {
        return a * (static_cast<Float>(1.0) - u) + b * u;
    }

    template <typename Float, FOR ref>
    struct Point final {
        Float x, y, z;
        Point operator+(Vector<Float, ref> rhs) const noexcept {
            return { x + rhs.x, y + rhs.y, z + rhs.z };
        }
        Point& operator+=(Vector<Float, ref> rhs) noexcept {
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
            return *this;
        }
        Point operator-(Vector<Float, ref> rhs) const noexcept {
            return { x - rhs.x, y - rhs.y, z - rhs.z };
        }
        Point& operator-=(Vector<Float, ref> rhs) noexcept {
            x -= rhs.x;
            y -= rhs.y;
            z -= rhs.z;
            return *this;
        }
        Vector<Float, ref> operator-(Point rhs) const noexcept {
            return { x - rhs.x, y - rhs.y, z - rhs.z };
        }
    };

    template <typename Float, FOR ref>
    Point<Float, ref> lerp(Point<Float, ref> a, Point<Float, ref> b, Float u) {
        return a * (static_cast<Float>(1.0) - u) + b * u;
    }

    template <typename Float, FOR ref>
    Float distanceSquared(Point<Float, ref> a, Point<Float, ref> b) {
        return lengthSquared(a - b);
    }

    template <typename Float, FOR ref>
    Float distance(Point<Float, ref> a, Point<Float, ref> b) {
        return length(a - b);
    }

    template <typename Float, FOR ref>
    struct Normal final {
        Dimensionless<Float> x, y, z;
        Normal() = default;
        Normal(Vector<Dimensionless<Float>, ref> v, Unchecked) : x(v.x), y(v.y), z(v.z) {}
        template <typename U>
        explicit Normal(Vector<U, ref> v) noexcept {
            auto nv = v / length(v);
            x = nv.x, y = nv.y, z = nv.z;
        }
        template <typename U>
        Vector<U, ref> operator*(U distance) const noexcept {
            return Vector<U, ref>{ x * distance, y * distance, z * distance };
        }
        Normal operator-() const noexcept {
            return { Vector<Dimensionless<Float>, ref>{ -x, -y, -z }, Unchecked{} };
        }
    };

    template <typename Float, FOR ref>
    Normal<Float, ref> cross(Normal<Float, ref> a, Normal<Float, ref> b) noexcept {
        return { Vector<Dimensionless<Float>, ref>{ dot(a.y, b.z) - dot(a.z, b.y), dot(a.z, b.x) - dot(a.x, b.z),
                                                    dot(a.x, b.y) - dot(a.y, b.x) },
                 Unchecked{} };
    }

    template <typename Float, FOR ref>
    Dimensionless<Float> dot(Normal<Float, ref> a, Normal<Float, ref> b) noexcept {
        return dot(a.x, b.x) + dot(a.y, b.y) + dot(a.z, b.z);
    }

    template <typename T, FOR ref, typename U>
    T dot(Vector<T, ref> a, Normal<U, ref> b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    template <typename Float, FOR ref>
    Normal<Float, ref> halfVector(Normal<Float, ref> a, Normal<Float, ref> b) {
        return Normal<Float, ref>{ Vector<Dimensionless<Float>, ref>{ a.x + b.x, a.y + b.y, a.z + b.z } };
    }

    template <typename Float, FOR ref>
    Dimensionless<Float> cosTheta(Normal<Float, ref> a) {
        return a.z;
    }

    template <typename T, FOR ref>
    auto normalize(Vector<T, ref> v) {
        return Normal<typename T::FT, ref>{ v };
    }

    // TODO:Quaternion
    template <typename Float, FOR refA, FOR refB>
    struct Transform final {
        using Storage = Dimensionless<typename Float::FT>;
        Storage A2B[3][4], B2A[3][4];
        Vector<Float, refB> operator()(Vector<Float, refA> v) const noexcept {
            return { A2B[0][0] * v.x + A2B[0][1] * v.y + A2B[0][2] * v.z, A2B[1][0] * v.x + A2B[1][1] * v.y + A2B[1][2] * v.z,
                     A2B[2][0] * v.x + A2B[2][1] * v.y + A2B[2][2] * v.z };
        }

        Point<Float, refB> operator()(Point<Float, refA> p) const noexcept {
            return { A2B[0][0] * p.x + A2B[0][1] * p.y + A2B[0][2] * p.z + A2B[0][3],
                     A2B[1][0] * p.x + A2B[1][1] * p.y + A2B[1][2] * p.z + A2B[1][3],
                     A2B[2][0] * p.x + A2B[2][1] * p.y + A2B[2][2] * p.z + A2B[2][3] };
        }

        Normal<typename Storage::FT, refB> operator()(Normal<typename Storage::FT, refA> n) const noexcept {
            return { Vector<Storage, refB>{ B2A[0][0] * n.x + B2A[1][0] * n.y + B2A[2][0] * n.z,
                                            B2A[0][1] * n.x + B2A[1][1] * n.y + B2A[2][1] * n.z,
                                            B2A[0][2] * n.x + B2A[1][2] * n.y + B2A[2][2] * n.z },
                     Unchecked{} };
        }

        Vector<Float, refA> operator()(Vector<Float, refB> v) const noexcept {
            return { B2A[0][0] * v.x + B2A[0][1] * v.y + B2A[0][2] * v.z, B2A[1][0] * v.x + B2A[1][1] * v.y + B2A[1][2] * v.z,
                     B2A[2][0] * v.x + B2A[2][1] * v.y + B2A[2][2] * v.z };
        }

        Point<Float, refA> operator()(Point<Float, refB> p) const noexcept {
            return { B2A[0][0] * p.x + B2A[0][1] * p.y + B2A[0][2] * p.z + B2A[0][3],
                     B2A[1][0] * p.x + B2A[1][1] * p.y + B2A[1][2] * p.z + B2A[1][3],
                     B2A[2][0] * p.x + B2A[2][1] * p.y + B2A[2][2] * p.z + B2A[2][3] };
        }

        Normal<typename Storage::FT, refA> operator()(Normal<typename Storage::FT, refB> n) const noexcept {
            return { Vector<Storage, refA>{ A2B[0][0] * n.x + A2B[1][0] * n.y + A2B[2][0] * n.z,
                                            A2B[0][1] * n.x + A2B[1][1] * n.y + A2B[2][1] * n.z,
                                            A2B[0][2] * n.x + A2B[1][2] * n.y + A2B[2][2] * n.z },
                     Unchecked{} };
        }
    };

    // TODO:optimize
    template <typename Float>
    void calcInverse(const Float A2B[3][4], Float B2A[3][4]) noexcept {
        // y=Ax+b
        // x=inv(A)*y-inv(A)*b
        auto det = A2B[0][0] * A2B[1][1] * A2B[2][2] + A2B[0][1] * A2B[1][2] * A2B[2][0] + A2B[0][2] * A2B[1][0] * A2B[2][1] -
            A2B[2][0] * A2B[1][1] * A2B[0][2] - A2B[2][1] * A2B[1][2] * A2B[0][0] - A2B[2][2] * A2B[1][0] * A2B[0][1];
        B2A[0][0] = A2B[0][0] * (A2B[1][1] * A2B[2][2] - A2B[1][2] * A2B[2][1]) / det;
        B2A[0][1] = -A2B[1][0] * (A2B[0][1] * A2B[2][2] - A2B[0][2] * A2B[2][1]) / det;
        B2A[0][2] = A2B[2][0] * (A2B[0][1] * A2B[1][2] - A2B[0][2] * A2B[1][1]) / det;
        B2A[1][0] = -A2B[0][1] * (A2B[1][0] * A2B[2][2] - A2B[1][2] * A2B[2][0]) / det;
        B2A[1][1] = A2B[1][1] * (A2B[0][0] * A2B[2][2] - A2B[0][2] * A2B[2][0]) / det;
        B2A[1][2] = -A2B[2][1] * (A2B[0][0] * A2B[1][2] - A2B[0][2] * A2B[1][0]) / det;
        B2A[2][0] = A2B[0][2] * (A2B[1][0] * A2B[2][1] - A2B[1][1] * A2B[2][0]) / det;
        B2A[2][1] = -A2B[1][2] * (A2B[0][0] * A2B[2][1] - A2B[0][1] * A2B[2][0]) / det;
        B2A[2][2] = A2B[2][2] * (A2B[0][0] * A2B[1][1] - A2B[0][1] * A2B[1][0]) / det;

        B2A[3][0] = -(B2A[0][0] * A2B[3][0] + B2A[0][1] * A2B[3][1] + B2A[0][2] * A2B[3][2]);
        B2A[3][1] = -(B2A[1][0] * A2B[3][0] + B2A[1][1] * A2B[3][1] + B2A[1][2] * A2B[3][2]);
        B2A[3][2] = -(B2A[2][0] * A2B[3][0] + B2A[2][1] * A2B[3][1] + B2A[2][2] * A2B[3][2]);
    }

    template <typename Float, FOR ref>
    Normal<Float, ref> reflect(Normal<Float, ref> in, Normal<Float, ref> N) noexcept {}

    template <typename Float, FOR ref>
    bool refract(Normal<Float, ref> in, Normal<Float, ref> N, Float ior, Normal<Float, ref>& out) noexcept {}

    template <typename Float, FOR ref>
    Vector<Float, ref> faceforward(Vector<Float, ref> in, Normal<Float, ref> N) noexcept {}

}  // namespace Piper