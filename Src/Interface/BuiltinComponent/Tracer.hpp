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
#include "../../Kernel/Protocol.hpp"
#include "../../STL/Optional.hpp"
#include "../../STL/Variant.hpp"
#include "../Infrastructure/Allocator.hpp"
#include "../Infrastructure/Concurrency.hpp"
#include "../Object.hpp"

namespace Piper {
    class AccelerationStructure : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(AccelerationStructure, Object)
        virtual ~AccelerationStructure() = default;
    };

    class Pipeline : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Pipeline, Object)
        virtual ~Pipeline() = default;
    };

    class Node : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Node, Object)
        virtual ~Node() = default;
    };

    class RTProgram : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RTProgram, Object)
        virtual ~RTProgram() = default;
    };

    struct TriangleIndexedGeometryDesc final {
        Optional<Transform<Distance, FOR::Local, FOR::World>> transform;
        uint32_t vertCount, triCount, stride;
        Ptr vertices;
        Ptr index;
    };

    enum class PrimitiveShapeType { TriangleIndexed };
    struct GeometryDesc final {
        PrimitiveShapeType type;
        union {
            TriangleIndexedGeometryDesc triangleIndexed;
        };
    };

    struct GSMInstanceDesc final {
        SharedPtr<Geometry> geometry;
        SharedPtr<Surface> surface;
        SharedPtr<Medium> medium;
        Optional<Transform<Distance, FOR::Local, FOR::World>> transform;
    };
    struct NodeInstanceDesc final {
        SharedPtr<Node> node;
        Optional<Transform<Distance, FOR::Local, FOR::World>> transform;
    };

    using NodeDesc = Variant<DynamicArray<NodeInstanceDesc>, GSMInstanceDesc>;

    using SBTPayload = DynamicArray<std::byte>;
    template <typename T, typename = std::enable_if_t<std::is_trivial_v<T>>>
    SBTPayload packSBTPayload(STLAllocator allocator, const T& data) {
        return SBTPayload{ reinterpret_cast<const std::byte*>(&data), reinterpret_cast<const std::byte*>(&data) + sizeof(data),
                           allocator };
    }

    struct RenderRECT final {
        uint32_t left, top, width, height;
    };

    // TODO:Texture extension for Accelerator
    // TODO:Concurrency
    class Tracer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Tracer, Object)
        virtual ~Tracer() = default;
        // TODO:update structure
        virtual SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) = 0;
        virtual SharedPtr<Node> buildNode(const NodeDesc& desc) = 0;
        // TODO:callee
        virtual SharedPtr<RTProgram> buildProgram(Future<LinkableProgram> linkable, String symbol) = 0;
        virtual UniqueObject<Pipeline> buildPipeline(SharedPtr<Node> scene, Sensor& sensor, Environment& environment,
                                                     Integrator& integrator, RenderDriver& renderDriver, Light& light,
                                                     Sampler* sampler, uint32_t width, uint32_t height) = 0;
        virtual Accelerator& getAccelerator() = 0;
        virtual ResourceCacheManager& getCacheManager() = 0;
        virtual void trace(Pipeline& pipeline, const RenderRECT& rect, const SBTPayload& renderDriverPayload,
                           const SensorNDCAffineTransform& transform, uint32_t sample) = 0;
    };
}  // namespace Piper
