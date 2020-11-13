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

    enum class FOR { Camera, World, Local, Shading };

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

        Vector operator*(Float rhs) const noexcept {
            return Vector{ x * rhs, y * rhs, z * rhs };
        }
        Vector& operator*=(Float rhs) noexcept {
            x *= rhs;
            y *= rhs;
            z *= rhs;
            return *this;
        }

        Vector operator/(Float rhs) const noexcept {
            return Vector{ x / rhs, y / rhs, z / rhs };
        }
        Vector& operator/=(Float rhs) noexcept {
            x /= rhs;
            y /= rhs;
            z /= rhs;
            return *this;
        }
    };

    template <typename Float, FOR ref>
    Vector<Float, ref> operator*(Float lhs, Vector<Float, ref> rhs) noexcept {
        return rhs * lhs;
    }

    template <typename Float, FOR ref>
    Float dot(Vector<Float, ref> a, Vector<Float, ref> b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    template <typename Float, FOR ref>
    Vector<Float, ref> cross(Vector<Float, ref> a, Vector<Float, ref> b) noexcept {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    template <typename Float, FOR ref>
    Float lengthSquared(Vector<Float, ref> a) noexcept {
        return a.x * a.x + a.y * a.y + a.z * a.z;
    }

    template <typename Float, FOR ref>
    Float length(Vector<Float, ref> a) noexcept {
        return std::sqrt(lengthSquared(a));
    }

    template <typename Float, FOR ref>
    Vector<Float, ref> normalize(Vector<Float, ref> a) noexcept {
        return a / length(a);
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
        Float x, y, z;

        explicit Normal(Vector<Float, ref> v) noexcept {
            v = normalize(v);
            x = v.x, y = v.y, z = v.z;
        }
    };

    // TODO:Quaternion
    template <typename Float, FOR refA, FOR refB>
    struct Transform final {
        Float A2B[4][4], B2A[4][4];
        Vector<Float, refB> operator()(Vector<Float, refA> v) const noexcept {
            return { A2B[0][0] * v.x + A2B[0][1] * v.y + A2B[0][2] * v.z, A2B[1][0] * v.x + A2B[1][1] * v.y + A2B[1][2] * v.z,
                     A2B[2][0] * v.x + A2B[2][1] * v.y + A2B[2][2] * v.z };
        }
        Point<Float, refB> operator()(Point<Float, refA> p) const noexcept {
            return { A2B[0][0] * p.x + A2B[0][1] * p.y + A2B[0][2] * p.z + A2B[0][3],
                     A2B[1][0] * p.x + A2B[1][1] * p.y + A2B[1][2] * p.z + A2B[1][3],
                     A2B[2][0] * p.x + A2B[2][1] * p.y + A2B[2][2] * p.z + A2B[2][3] };
        }
        Normal<Float, refB> operator()(Normal<Float, refA> n) const noexcept {
            return { B2A[0][0] * n.x + B2A[1][0] * n.y + B2A[2][0] * n.z, B2A[0][1] * n.x + B2A[1][1] * n.y + B2A[2][1] * n.z,
                     B2A[0][2] * n.x + B2A[1][2] * n.y + B2A[2][2] * n.z };
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
        Normal<Float, refA> operator()(Normal<Float, refB> n) const noexcept {
            return { A2B[0][0] * n.x + A2B[1][0] * n.y + A2B[2][0] * n.z, A2B[0][1] * n.x + A2B[1][1] * n.y + A2B[2][1] * n.z,
                     A2B[0][2] * n.x + A2B[1][2] * n.y + A2B[2][2] * n.z };
        }
    };

    template <typename Float, FOR ref>
    Normal<Float, ref> reflect(Normal<Float, ref> in, Normal<Float, ref> N) noexcept {}

    template <typename Float, FOR ref>
    bool refract(Normal<Float, ref> in, Normal<Float, ref> N, Float ior, Normal<Float, ref>& out) noexcept {}

    template <typename Float, FOR ref>
    Vector<Float, ref> faceforward(Vector<Float, ref> in, Normal<Float, ref> N) noexcept {}

}  // namespace Piper
