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

#include "../../Kernel/Transform.hpp"
#include "../../STL/Function.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/UniquePtr.hpp"
#include "../Infrastructure/Allocator.hpp"
#include "../Infrastructure/Concurrency.hpp"
#include "../Object.hpp"

namespace Piper {
    // https://raytracing-docs.nvidia.com/optix7/guide/index.html#preface#terms-used-in-this-document
    enum class RTProgramType { RayGeneration, Intersection, HitGroup, Miss, DirectCallable, ContinuationCallable };
    enum class PrimitiveShapeType { Triangle, TriangleIndexed, Instance };

    class PITU;
    class Accelerator;

    class Acceleration : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Acceleration, Object)
        virtual ~Acceleration() = default;
    };

    class RTProgram : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RTProgram, Object)
        virtual RTProgramType type() const noexcept = 0;
        virtual ~RTProgram() = default;
    };

    class Pipeline : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Pipeline, Object)
        virtual ~Pipeline() = default;
    };

    class Geometry;
    class Material;
    class Medium;

    struct Instance final {
        SharedPtr<Geometry> geometry;
        SharedPtr<Material> material;
        SharedPtr<Medium> medium;
        Transform<float, FOR::Local, FOR::World> transform;
    };

    struct TriangleGeometryDesc final {
        uint32_t count, strides;
        Ptr vertices;
    };

    struct TriangleIndexedGeometryDesc final {
        uint32_t count, strides;
        Ptr vertices;
        Ptr index;
    };

    struct InstanceGeometryDesc final {
        DynamicArray<Instance> instances;
    };

    struct GeometryDesc final {
        PrimitiveShapeType type;
        union {
            TriangleGeometryDesc triangle;
            TriangleIndexedGeometryDesc triangleIndexed;
            InstanceGeometryDesc instance;
        };
    };

    // https://raytracing-docs.nvidia.com/optix7/guide/index.html#shader_binding_table#shader-binding-table
    using SBTPayload = DynamicArray<std::byte>;
    struct SBTRecord final {
        RTProgram* program;
        SBTPayload payload;
    };
    struct HitGroup final {
        SBTRecord closestHit;
        SBTRecord anyHit;
    };
    struct SBT final {
        Acceleration* root;
        SBTRecord raygen;
        DynamicArray<SBTRecord> missing;
        DynamicArray<HitGroup> hitGroup;
        DynamicArray<SBTRecord> directCallable;
        DynamicArray<SBTRecord> continuationCallable;
    };

    // TODO:Texture extension for Accelerator
    class Tracer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Tracer, Object)
        virtual ~Tracer() = default;
        // TODO:update structure
        virtual UniquePtr<Acceleration> buildAcceleration(const GeometryDesc& desc) = 0;
        virtual UniquePtr<RTProgram> buildProgram(Function<UniquePtr<PITU>, Accelerator&> src, RTProgramType type) = 0;
        virtual UniquePtr<Pipeline> buildPipeline(DynamicArray<UniquePtr<RTProgram>> programs) = 0;
        virtual Future<void> trace(Pipeline& pipeline, const SBT& SBT) = 0;
    };
}  // namespace Piper
