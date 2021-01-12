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
#include "../../STL/Function.hpp"
#include "../../STL/Optional.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/Variant.hpp"
#include "../Infrastructure/Allocator.hpp"
#include "../Infrastructure/Concurrency.hpp"
#include "../Object.hpp"

namespace Piper {
    enum class TextureWrap : uint32_t;

    class AccelerationStructure : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(AccelerationStructure, Object)
        virtual ~AccelerationStructure() = default;
    };

    class Pipeline : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Pipeline, Object) [[nodiscard]] virtual String generateStatisticsReport() const = 0;
        [[nodiscard]] virtual uint32_t getSamplesPerPixel() const noexcept = 0;
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
        uint32_t vertCount, triCount;
        Ptr vertices;
        Ptr index;

        // optional
        Ptr texCoords;
        Ptr normal;
        Ptr tangent;
    };

    struct CustomGeometryDesc final {
        uint32_t count;
        Ptr bounds;
    };

    enum class PrimitiveShapeType { TriangleIndexed, Custom };
    struct GeometryDesc final {
        PrimitiveShapeType type;
        Optional<Transform<Distance, FOR::Local, FOR::World>> transform;
        union {
            TriangleIndexedGeometryDesc triangleIndexed;
            CustomGeometryDesc custom;
        };
    };

    class GSMInstance : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(GSMInstance, Object);
        virtual ~GSMInstance() = default;
    };

    using TransformInfo = DynamicArray<Pair<Time<float>, Transform<Distance, FOR::Local, FOR::World>>>;

    using SBTPayload = DynamicArray<std::byte>;
    template <typename T, typename = std::enable_if_t<std::is_trivial_v<T>>>
    SBTPayload packSBTPayload(STLAllocator allocator, const T& data) {
        return SBTPayload{ reinterpret_cast<const std::byte*>(&data), reinterpret_cast<const std::byte*>(&data) + sizeof(data),
                           allocator };
    }

    struct RenderRECT final {
        uint32_t left, top, width, height;
    };

    using CallSiteRegister = Function<CallHandle, const SharedPtr<RTProgram>&, const SBTPayload&>;
    using TextureLoader = Function<CallHandle, const SharedPtr<Config>&, uint32_t>;
    struct MaterializeContext final {
        Tracer& tracer;
        ResourceHolder& holder;
        MemoryArena& arena;
        const CallSiteRegister registerCall;
        Profiler& profiler;
        const TextureLoader loadTexture;
    };

    // TODO:Texture extension for Accelerator
    // TODO:Concurrency
    class Tracer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Tracer, Object)
        virtual ~Tracer() = default;
        // TODO:update structure
        virtual SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) = 0;
        virtual SharedPtr<GSMInstance> buildGSMInstance(SharedPtr<Geometry> geometry, SharedPtr<Surface> surface,
                                                        SharedPtr<Medium> medium) = 0;
        virtual SharedPtr<Node> buildNode(const SharedPtr<Object>& object) = 0;
        virtual SharedPtr<Node> buildNode(const DynamicArray<Pair<TransformInfo, SharedPtr<Node>>>& children) = 0;
        // TODO:call graph
        virtual SharedPtr<RTProgram> buildProgram(LinkableProgram linkable, String symbol) = 0;
        // TODO:better interface
        virtual UniqueObject<Pipeline> buildPipeline(const SharedPtr<Node>& scene, const SharedPtr<Node>& sensor,
                                                     Integrator& integrator, RenderDriver& renderDriver,
                                                     LightSampler& lightSampler, Sampler& sampler, uint32_t width,
                                                     uint32_t height, float& ratio) = 0;
        virtual Accelerator& getAccelerator() = 0;
        virtual ResourceCacheManager& getCacheManager() = 0;
        virtual void trace(Pipeline& pipeline, const RenderRECT& rect, const SBTPayload& renderDriverPayload,
                           const SensorNDCAffineTransform& transform, uint32_t sample) = 0;
        [[nodiscard]] virtual SharedPtr<Texture> generateTexture(const SharedPtr<Config>& textureDesc,
                                                                 uint32_t channel) const = 0;
    };
}  // namespace Piper
