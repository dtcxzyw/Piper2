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
#include "../../Kernel/Protocol.hpp"
#include "../../STL/Function.hpp"
#include "../../STL/Optional.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/Variant.hpp"
#include "../Infrastructure/Accelerator.hpp"
#include "../Infrastructure/Allocator.hpp"
#include "../Infrastructure/Concurrency.hpp"
#include "../Object.hpp"

namespace Piper {
    enum class TextureWrap : uint32_t;

    class AccelerationStructure : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(AccelerationStructure, Object)
    };

    class Node : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Node, Object)
    };

    class RTProgram : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RTProgram, Object)
    };

    constexpr auto invalidOffset = std::numeric_limits<size_t>::max();

    struct TriangleIndexedGeometryDesc final {
        uint32_t vertCount, triCount;
        SharedPtr<Buffer> buffer;
        // offset
        size_t vertices;
        size_t index;

        // optional
        size_t texCoords;
        size_t normal;
        size_t tangent;
    };

    struct CustomGeometryDesc final {
        uint32_t count;
        SharedPtr<Buffer> bounds;
    };

    struct GeometryDesc final {
        Optional<Transform<Distance, FOR::Local, FOR::World>> transform;
        Variant<TriangleIndexedGeometryDesc, CustomGeometryDesc> desc;
    };

    class GSMInstance : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(GSMInstance, Object)
    };

    struct TransformInfo final {
        Time<float> offset;
        Time<float> step;
        DynamicArray<Pair<uint32_t, TransformSRT>> transforms;  // local to world
    };

    using SBTPayload = Binary;
    template <typename T, typename = std::enable_if_t<std::is_trivial_v<T>>>
    SBTPayload packSBTPayload(const STLAllocator allocator, const T& data) {
        return SBTPayload{ reinterpret_cast<const std::byte*>(&data), reinterpret_cast<const std::byte*>(&data) + sizeof(data),
                           allocator };
    }

    struct RenderRECT final {
        uint32_t left, top, width, height;
    };

    // NOTICE: It is a guard.
    class TraceLauncher : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(TraceLauncher, Object) [[nodiscard]] virtual uint32_t getSamplesPerPixel() const noexcept = 0;
        virtual void updateTimeInterval(Time<float> begin, Time<float> end) noexcept = 0;
        [[nodiscard]] virtual Future<void> launch(const RenderRECT& rect, const Function<SBTPayload, uint32_t>& launchData,
                                                  const Span<ResourceView>& resources, const SensorNDCAffineTransform& transform,
                                                  uint32_t sampleCount) = 0;
    };

    class Pipeline : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Pipeline, Object) [[nodiscard]] virtual String generateStatisticsReport() const = 0;
        // TODO: better interface
        [[nodiscard]] virtual SharedPtr<TraceLauncher> prepare(const SharedPtr<Node>& sensor, uint32_t width, uint32_t height,
                                                               float& deviceAspectRatio) = 0;
    };

    using CallSiteRegister = Function<CallHandle, const SharedPtr<RTProgram>&, const SBTPayload&>;
    using TextureLoader = Function<CallHandle, const SharedPtr<Config>&, uint32_t>;
    using ResourceRegister = Function<uint32_t, SharedPtr<Resource>>;
    struct MaterializeContext final {
        Tracer& tracer;
        const ResourceRegister& registerResource;
        const CallSiteRegister registerCall;
        Profiler& profiler;
        const TextureLoader loadTexture;
    };

    // TODO: Texture extension for Accelerator
    // TODO: Concurrency
    // TODO: Alignment Requirement Query

    enum class AlignmentRequirement { VertexBuffer, IndexBuffer, TextureCoordsBuffer, BoundsBuffer };

    class Tracer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Tracer, Object)
        // TODO: update structure
        virtual SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) = 0;
        virtual SharedPtr<GSMInstance> buildGSMInstance(SharedPtr<Geometry> geometry, SharedPtr<Surface> surface,
                                                        SharedPtr<Medium> medium) = 0;
        virtual SharedPtr<Node> buildNode(const SharedPtr<Object>& object) = 0;
        virtual SharedPtr<Node> buildNode(const DynamicArray<Pair<TransformInfo, SharedPtr<Node>>>& children) = 0;
        // TODO: call graph?
        virtual SharedPtr<RTProgram> buildProgram(LinkableProgram linkable, String symbol) = 0;
        // TODO: better interface
        virtual UniqueObject<Pipeline> buildPipeline(const SharedPtr<Node>& scene, Integrator& integrator,
                                                     RenderDriver& renderDriver, LightSampler& lightSampler,
                                                     SharedPtr<Sampler> sampler) = 0;
        virtual Accelerator& getAccelerator() = 0;
        virtual ResourceCacheManager& getCacheManager() = 0;
        [[nodiscard]] virtual SharedPtr<Texture> generateTexture(const SharedPtr<Config>& textureDesc,
                                                                 uint32_t channel) const = 0;
        [[nodiscard]] virtual size_t getAlignmentRequirement(AlignmentRequirement requirement) const noexcept = 0;
    };
}  // namespace Piper
