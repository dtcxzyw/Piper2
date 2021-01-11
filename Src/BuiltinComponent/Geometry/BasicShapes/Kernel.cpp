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
#include <limits>

namespace Piper {
    struct PlaneStorage final {
        const PerPlaneData* plane;
        Vector2<float> uv;
        Face face;
    };
    static_assert(sizeof(PlaneStorage) <= sizeof(GeometryStorage));

    extern "C" Vector2<float> calcTexCoord(const Vector<Distance, FOR::Local>& pos, const PerPlaneData& data) {
        // ignore round-off error
        // u*x+v*y=hitPos
        Inverse<Area<float>> invDet;
        Area<float> x, y;
        switch(data.maxDetComp) {
            case 0: {
                // YZ
                invDet = inverse(data.u.y * data.v.z - data.v.y * data.u.z);
                x = pos.y * data.v.z - data.v.y * pos.z;
                y = data.u.y * pos.z - pos.y * data.u.z;
            } break;
            case 1: {
                // ZX
                invDet = inverse(data.u.z * data.v.x - data.v.z * data.u.x);
                x = pos.z * data.v.x - data.v.z * pos.x;
                y = data.u.z * pos.x - pos.z * data.u.x;
            } break;
            default: {
                // XY
                invDet = inverse(data.u.x * data.v.y - data.v.x * data.u.y);
                x = pos.x * data.v.y - data.v.x * pos.y;
                y = data.u.x * pos.y - pos.x * data.u.y;
            } break;
        }
        return { (x * invDet).val, (y * invDet).val };
    }

    // TODO:handle degeneracy
    extern "C" void planeIntersect(RestrictedContext, const void* SBTData, const uint32_t primitiveID,
                                   const RayInfo<FOR::Local>& ray, const float tNear, float& tFar, void* storage) {
        const auto& data = static_cast<const PlaneData*>(SBTData)->primitives[primitiveID];
        // dot(ray.origin + ray.direction * t - plane.origin, plane.normal) = 0 -> kt=b
        const auto delta = data.origin - ray.origin;
        const auto b = dot(delta, data.normal);
        const auto k = dot(ray.direction, data.normal);
        const auto t = b / k;
        if(tNear < t.val && t.val < tFar) {
            const auto hitPos = ray.direction * t - delta;
            const auto uv = calcTexCoord(hitPos, data);
            if(0.0f <= uv.x && uv.x <= 1.0f && 0.0f <= uv.y && uv.y <= 1.0f) {
                tFar = t.val;
                auto& res = *static_cast<PlaneStorage*>(storage);
                res = PlaneStorage{ &data, uv, k.val > 0.0f ? Face::Front : Face::Back };
            }
        }
    }
    static_assert(std::is_same_v<GeometryIntersectFunc, decltype(&planeIntersect)>);

    extern "C" void planeOcclude(RestrictedContext, const void* SBTData, const uint32_t primitiveID,
                                 const RayInfo<FOR::Local>& ray, const float tNear, const float tFar, bool& hit) {
        const auto& data = static_cast<const PlaneData*>(SBTData)->primitives[primitiveID];
        // dot(ray.origin + ray.direction * t - plane.origin, plane.normal) = 0 -> kt=b
        const auto delta = data.origin - ray.origin;
        const auto b = dot(delta, data.normal);
        const auto k = dot(ray.direction, data.normal);
        const auto t = b / k;
        hit = (tNear * k.val < b.val && b.val < tFar * k.val);
        if(tNear < t.val && t.val < tFar) {
            const auto hitPos = ray.direction * (b / k) - delta;
            const auto uv = calcTexCoord(hitPos, data);
            hit = 0.0f <= uv.x && uv.x <= 1.0f && 0.0f <= uv.y && uv.y <= 1.0f;
        } else
            hit = false;
    }
    static_assert(std::is_same_v<GeometryOccludeFunc, decltype(&planeOcclude)>);

    extern "C" void planeSurface(RestrictedContext, const void*, const void* storage,
                                 SurfaceIntersectionInfo& info) {
        const auto& data = *static_cast<const PlaneStorage*>(storage);
        info.face = data.face;
        info.Ng = data.face == Face::Front ? -data.plane->normal : data.plane->normal;
        // TODO:bump/normal map
        info.N = info.Ng;
        info.T = data.plane->tangent;
        info.B = cross(info.N, info.T);
        info.texCoord = data.uv;
    }
    static_assert(std::is_same_v<GeometryPostProcessFunc, decltype(&planeSurface)>);
}  // namespace Piper
