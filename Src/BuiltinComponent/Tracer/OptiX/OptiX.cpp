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

#define PIPER_EXPORT
#include "../../../Interface/BuiltinComponent/Geometry.hpp"
#include "../../../Interface/BuiltinComponent/Image.hpp"
#include "../../../Interface/BuiltinComponent/Integrator.hpp"
#include "../../../Interface/BuiltinComponent/Light.hpp"
#include "../../../Interface/BuiltinComponent/RenderDriver.hpp"
#include "../../../Interface/BuiltinComponent/Sampler.hpp"
#include "../../../Interface/BuiltinComponent/Sensor.hpp"
#include "../../../Interface/BuiltinComponent/Surface.hpp"
#include "../../../Interface/BuiltinComponent/Texture.hpp"
#include "../../../Interface/BuiltinComponent/Tracer.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Profiler.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#include "../../../Kernel/Protocol.hpp"
#include "../../../STL/Map.hpp"
#include "Shared.hpp"
#pragma warning(push, 0)
#include <cuda.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>
#pragma warning(pop)
#include <utility>

namespace Piper {
    // TODO: NVLink
    // TODO: need makeCurrent?

    static void checkCUDAResult(PiperContext& context, const SourceLocation& loc, const CUresult res) {
        if(res == CUDA_SUCCESS)
            return;
        auto name = "UNKNOWN", str = "Unknown error";
        cuGetErrorName(res, &name);
        cuGetErrorString(res, &str);
        context.getErrorHandler().raiseException(String{ "CUDA Error[", context.getAllocator() } + name + "] " + str + ".", loc);
    }

    static void checkOptixResult(PiperContext& context, const SourceLocation& loc, const OptixResult res) {
        if(res == OPTIX_SUCCESS)
            return;
        const auto name = optixGetErrorName(res);
        const auto str = optixGetErrorString(res);
        context.getErrorHandler().raiseException(String{ "OptiX Error[", context.getAllocator() } + name + "] " + str + ".", loc);
    }

    constexpr auto gsmMask = 1, areaLightMask = 2;

    struct OptixTraversalNode final {
        const OptixTraversalNode* parent;
        OptixTraversableHandle geometry;
    };

    struct DeviceDeleter {
        Context* ctx;
        void operator()(const OptixDeviceContext context) const {
            checkOptixResult(ctx->context(), PIPER_SOURCE_LOCATION(), optixDeviceContextDestroy(context));
        }
    };
    using DeviceHandle = UniquePtr<OptixDeviceContext_t, DeviceDeleter>;
    // NOTICE: optixBuiltinISModuleGet

    class OptixModuleHolder final : public ResourceInstance {
    private:
        Context& mContext;
        OptixModule mModule;

    public:
        OptixModuleHolder(Context& context, const OptixDeviceContext device, const OptixModuleCompileOptions& MCO,
                          const OptixPipelineCompileOptions& PCO, const String& ptx)
            : ResourceInstance{ context.context() }, mContext{ context } {
            auto guard = context.makeCurrent();

            char logBuffer[1024];
            size_t logSize = sizeof(logBuffer);

            checkOptixResult(
                context.context(), PIPER_SOURCE_LOCATION(),
                optixModuleCreateFromPTX(device, &MCO, &PCO, ptx.c_str(), ptx.size(), logBuffer, &logSize, &mModule));
            // TODO: print log
            // TODO: OptixModuleCompileOptions::boundValues;
        }
        ~OptixModuleHolder() override {
            auto guard = mContext.makeCurrent();
            checkOptixResult(context(), PIPER_SOURCE_LOCATION(), optixModuleDestroy(mModule));
        }
        [[nodiscard]] OptixModule getModuleHandle() const noexcept {
            return mModule;
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            return reinterpret_cast<ResourceHandle>(mModule);
        }
    };

    class OptixRTProgram : public Resource {
    private:
        Accelerator& mAccelerator;

    public:
        OptixRTProgram(PiperContext& context, Accelerator& accelerator) : Resource{ context }, mAccelerator{ accelerator } {}
        ResourceShareMode getShareMode() const noexcept override {
            return ResourceShareMode::Unique;
        }
        void flushBeforeRead(const Instance& dest) const override {}
        Instance instantiateMain() const override {
            return {};
        }
        Instance instantiateReference(Context* ctx) const override {
            return {};
        }
    };

    struct CustomBuffer final {
        const void* payload;
        GeometryIntersectFunc intersect;
        GeometryOccludeFunc occlude;
    };

    // TODO: sub class
    // TODO: treat as Resource
    class OptixAcceleration final : public AccelerationStructure {
    private:
        DynamicArray<OptixTraversableHandle> mGeometry;

        // CustomBuffer mCustomBuffer;
        SharedPtr<Buffer> mResource;

    public:
        OptixAcceleration(PiperContext& context, const DynamicArray<Pair<Context*, DeviceHandle>>& devices,
                          const GeometryDesc& desc)
            : AccelerationStructure{ context }, mGeometry{ context.getAllocator() } {

            // TODO: compact
            OptixAccelBuildOptions opt{ OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_RANDOM_VERTEX_ACCESS
                                        //    |OPTIX_BUILD_FLAG_ALLOW_UPDATE
                                        ,
                                        OPTIX_BUILD_OPERATION_BUILD,
                                        // TODO: motion blur
                                        OptixMotionOptions{ 1, OPTIX_MOTION_FLAG_NONE, 0.0f, 0.0f } };

            auto build = [&context, &opt](Context* ctx, OptixDeviceContext device, const OptixBuildInput& input) {
                OptixAccelBufferSizes sizes;
                checkOptixResult(context, PIPER_SOURCE_LOCATION(), optixAccelComputeMemoryUsage(device, &opt, &input, 1, &sizes));
                const auto stream = reinterpret_cast<CUstream>(ctx->select());
                // TODO: RAII
                // TODO: alignment
                CUdeviceptr tempBuffer, outputBuffer, AABBBuffer;
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuMemAllocAsync(&tempBuffer, sizes.tempSizeInBytes, stream));
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                cuMemAllocAsync(&outputBuffer, sizes.outputSizeInBytes, stream));
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuMemAllocAsync(&AABBBuffer, sizeof(OptixAabb), stream));
                OptixTraversableHandle handle;
                OptixAccelEmitDesc descs[1] = { { AABBBuffer, OPTIX_PROPERTY_TYPE_AABBS } };
                checkOptixResult(context, PIPER_SOURCE_LOCATION(),
                                 optixAccelBuild(device, reinterpret_cast<CUstream>(ctx->select()), &opt, &input, 1, tempBuffer,
                                                 sizes.tempSizeInBytes, outputBuffer, sizes.outputSizeInBytes, &handle, descs,
                                                 static_cast<uint32_t>(std::size(descs))));
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuMemFreeAsync(tempBuffer, stream));

                OptixAabb aabb;
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                cuMemcpyDtoHAsync(&aabb, AABBBuffer, sizeof(OptixAabb), stream));
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuMemFreeAsync(AABBBuffer, stream));
                // TODO: concurrency
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuStreamSynchronize(stream));
                return std::make_tuple(handle, outputBuffer, aabb);
            };

            switch(desc.desc.index()) {
                case 0: {
                    auto&& triDesc = eastl::get<TriangleIndexedGeometryDesc>(desc.desc);

                    // TODO: concurrency
                    triDesc.buffer->access()->wait();
                    mResource = triDesc.buffer;

                    // TODO: relocate
                    for(auto& [ctx, device] : devices) {
                        auto guard = ctx->makeCurrent();
                        // TODO: batch building
                        const auto bufferPtr = static_cast<CUdeviceptr>(triDesc.buffer->require(ctx)->getHandle());

                        const auto vertices = bufferPtr + triDesc.vertices;
                        // TODO: SBT
                        const OptixBuildInput input{ OPTIX_BUILD_INPUT_TYPE_TRIANGLES,
                                                     { OptixBuildInputTriangleArray{
                                                         &vertices, 1, OPTIX_VERTEX_FORMAT_FLOAT3, 3 * sizeof(float),
                                                         bufferPtr + triDesc.index, triDesc.triCount,
                                                         OPTIX_INDICES_FORMAT_UNSIGNED_INT3, 3 * sizeof(uint32_t), 0, nullptr, 0,
                                                         0, 0, 0, 0, OPTIX_TRANSFORM_FORMAT_NONE } } };

                        auto [handle, accel, aabb] = build(ctx, device.get(), input);

                        BuiltinTriangleBuffer buffer;
                        buffer.index = reinterpret_cast<const uint32_t*>(bufferPtr + triDesc.index);

                        if(triDesc.texCoords != invalidOffset)
                            buffer.texCoord = reinterpret_cast<const Vector2<float>*>(bufferPtr + triDesc.texCoords);
                        else
                            buffer.texCoord = nullptr;

                        if(triDesc.normal != invalidOffset)
                            buffer.Ns = reinterpret_cast<const Vector<float, FOR::Local>*>(bufferPtr + triDesc.normal);
                        else
                            buffer.Ns = nullptr;

                        if(triDesc.tangent != invalidOffset)
                            buffer.Ts = reinterpret_cast<const Vector<float, FOR::Local>*>(bufferPtr + triDesc.tangent);
                        else
                            buffer.Ts = nullptr;

                        // TODO: store buffer
                        context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
                    }
                } break;
                case 1: {
                    const auto& custom = eastl::get<CustomGeometryDesc>(desc.desc);
                    // TODO: concurrency
                    custom.bounds->access()->wait();
                    mResource = custom.bounds;

                    for(auto& [ctx, device] : devices) {
                        auto guard = ctx->makeCurrent();
                        const auto bounds = static_cast<CUdeviceptr>(custom.bounds->require(ctx)->getHandle());

                        // TODO: SBT
                        OptixBuildInput input;
                        input.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
                        input.customPrimitiveArray = { &bounds, custom.count, sizeof(OptixAabb), nullptr, 0, 0, 0, 0, 0 };

                        auto [handle, accel, aabb] = build(ctx, device.get(), input);
                    }
                } break;
                default:
                    context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            }
        }
        [[nodiscard]] auto&& getGeometry() const noexcept {
            return mGeometry;
        }
    };  // namespace Piper

    struct EmbreeGSMInstance final : public GSMInstance {
        SharedPtr<Geometry> geometry;
        SharedPtr<Surface> surface;
        SharedPtr<Medium> medium;

        EmbreeGSMInstance(PiperContext& context, SharedPtr<Geometry> geo, SharedPtr<Surface> surf, SharedPtr<Medium> med)
            : GSMInstance(context), geometry(std::move(geo)), surface(std::move(surf)), medium(std::move(med)) {}
    };

    class EmbreeLeafNodeWithGSM;
    struct GSMInstanceProgram final {
        SharedPtr<Geometry> geometry;
        SharedPtr<Surface> surface;
        SharedPtr<Medium> medium;
        TraversalHandle traversal;
        EmbreeLeafNodeWithGSM* node;
    };

    class EmbreeLeafNodeWithLight;
    struct LightInstanceProgram final {
        SharedPtr<Light> light;
        TraversalHandle traversal;
        EmbreeLeafNodeWithLight* node;
    };

    class EmbreeLeafNodeWithSensor;
    struct SensorInstanceProgram final {
        SharedPtr<Sensor> sensor;
        TraversalHandle traversal;
        EmbreeLeafNodeWithSensor* node;
    };

    class EmbreeNode : public Node {
    public:
        PIPER_INTERFACE_CONSTRUCT(EmbreeNode, Node);
        [[nodiscard]] virtual OptixTraversableHandle getTraversal() const noexcept = 0;
        virtual void collect(DynamicArray<GSMInstanceProgram>& gsm, DynamicArray<LightInstanceProgram>& light,
                             DynamicArray<SensorInstanceProgram>& sensor, const OptixTraversalNode* traversal,
                             MemoryArena& arena) = 0;
        virtual void updateTimeInterval(Time<float> begin, Time<float> end) {}
    };

    /*
    class EmbreeBranchNode final : public EmbreeNode {
    private:
        SceneHandle mScene;
        struct SubNode final {
            TransformInfo transform;
            GeometryHandle geometry;
            SharedPtr<EmbreeNode> node;
        };
        DynamicArray<SubNode> mChildren;

    public:
        EmbreeBranchNode(PiperContext& context, const RTCDevice device,
                         const DynamicArray<Pair<TransformInfo, SharedPtr<Node>>>& children)
            : EmbreeNode(context), mChildren{ context.getAllocator() } {
            mScene.reset(rtcNewScene(device));
            mChildren.reserve(children.size());
            for(auto&& [trans, child] : children) {
                mChildren.push_back(SubNode{ trans, GeometryHandle{ rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE) },
                                             eastl::dynamic_shared_pointer_cast<EmbreeNode>(child) });
                auto& sub = mChildren.back();
                rtcSetGeometryInstancedScene(sub.geometry.get(), sub.node->getScene());
                rtcSetGeometryUserData(sub.geometry.get(), sub.node->getScene());
                rtcSetGeometryMask(sub.geometry.get(), gsmMask | areaLightMask);
                // static transform
                if(trans.step.val <= 0.0f && !trans.transforms.empty()) {
                    if(trans.transforms.size() == 1 && trans.transforms.front().first == 0U)
                        setTransformSRT(sub.geometry.get(), 0, trans.transforms.front().second);
                    else
                        context.getErrorHandler().raiseException("Unrecognized transform.", PIPER_SOURCE_LOCATION());
                }
                rtcAttachGeometry(mScene.get(), sub.geometry.get());
            }
        }

        void updateTimeInterval(const Time<float> begin, const Time<float> end) override {
            for(auto&& [transform, geometry, child] : mChildren) {
                child->updateTimeInterval(begin, end);
                updateTransformSRT(geometry.get(), begin, end, transform);
                rtcCommitGeometry(geometry.get());
            }
            rtcCommitScene(mScene.get());
        }

        [[nodiscard]] RTCScene getScene() const noexcept override {
            return mScene.get();
        }

        void collect(DynamicArray<GSMInstanceProgram>& gsm, DynamicArray<LightInstanceProgram>& light,
                     DynamicArray<SensorInstanceProgram>& sensor, const EmbreeTraversalNode* traversal,
                     MemoryArena& arena) override {
            for(auto&& [trans, geometry, child] : mChildren) {
                auto* sub = arena.alloc<EmbreeTraversalNode>();
                sub->geometry = geometry.get();
                sub->parent = traversal;
                child->collect(gsm, light, sensor, sub, arena);
            }
        }
    };

    class EmbreeLeafNodeWithGSM final : public EmbreeNode {
    private:
        SceneHandle mScene;
        GeometryHandle mGeometry;
        SharedPtr<EmbreeGSMInstance> mInstance;
        SharedPtr<Wrapper<GSMInstanceUserData>> mUserData;

    public:
        EmbreeLeafNodeWithGSM(PiperContext& context, Tracer& tracer, Accelerator& accelerator, ResourceCacheManager& cacheManager,
                              const RTCDevice device, SharedPtr<EmbreeGSMInstance> instance)
            : EmbreeNode(context), mInstance(std::move(instance)),
              mUserData(makeSharedObject<Wrapper<GSMInstanceUserData>>(context)) {
            auto& accel =
                dynamic_cast<EmbreeAcceleration&>(mInstance->geometry->getAcceleration(tracer, accelerator, cacheManager));

            mGeometry.reset(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE));
            rtcSetGeometryInstancedScene(mGeometry.get(), accel.getScene(device));
            rtcSetGeometryUserData(mGeometry.get(), &mUserData->value);
            rtcSetGeometryMask(mGeometry.get(), gsmMask);
            rtcCommitGeometry(mGeometry.get());

            mScene.reset(rtcNewScene(device));
            rtcAttachGeometry(mScene.get(), mGeometry.get());
            rtcCommitScene(mScene.get());
        }

        [[nodiscard]] RTCScene getScene() const noexcept override {
            return mScene.get();
        }

        void collect(DynamicArray<GSMInstanceProgram>& gsm, DynamicArray<LightInstanceProgram>&,
                     DynamicArray<SensorInstanceProgram>&, const EmbreeTraversalNode* node, MemoryArena& arena) override {
            gsm.emplace_back(GSMInstanceProgram{ mInstance->geometry, mInstance->surface, nullptr,
                                                 reinterpret_cast<TraversalHandle>(node), this });
        }

        GSMInstanceUserData& postMaterialize(ResourceHolder& holder) const {
            holder.retain(mUserData);
            return mUserData->value;
        }
    };

    class EmbreeLeafNodeWithLight final : public EmbreeNode {
    private:
        GeometryHandle mGeometry;
        SceneHandle mScene;
        SharedPtr<Light> mLight;
        SharedPtr<Wrapper<AreaLightUserData>> mUserData;

    public:
        EmbreeLeafNodeWithLight(PiperContext& context, Tracer& tracer, Accelerator& accelerator,
                                ResourceCacheManager& cacheManager, const RTCDevice device, SharedPtr<Light> light)
            : EmbreeNode(context), mLight(std::move(light)) {
            const auto* geometry = mLight->getGeometry();
            mGeometry.reset(rtcNewGeometry(device, geometry ? RTC_GEOMETRY_TYPE_INSTANCE : RTC_GEOMETRY_TYPE_USER));
            if(geometry) {
                mUserData = makeSharedObject<Wrapper<AreaLightUserData>>(context);
                auto& accel = dynamic_cast<EmbreeAcceleration&>(geometry->getAcceleration(tracer, accelerator, cacheManager));
                rtcSetGeometryInstancedScene(mGeometry.get(), accel.getScene(device));
                rtcSetGeometryUserData(mGeometry.get(), &mUserData->value);
                rtcSetGeometryMask(mGeometry.get(), areaLightMask);
            } else {
                // dummy node
                rtcSetGeometryUserPrimitiveCount(mGeometry.get(), 0);
                rtcSetGeometryIntersectFunction(mGeometry.get(), [](auto) {});
                rtcSetGeometryBoundsFunction(
                    mGeometry.get(), [](auto) {}, nullptr);
                rtcSetGeometryOccludedFunction(mGeometry.get(), [](auto) {});
            }

            rtcCommitGeometry(mGeometry.get());

            mScene.reset(rtcNewScene(device));
            rtcAttachGeometry(mScene.get(), mGeometry.get());
            rtcCommitScene(mScene.get());
        }

        [[nodiscard]] RTCScene getScene() const noexcept override {
            return mScene.get();
        }

        void collect(DynamicArray<GSMInstanceProgram>&, DynamicArray<LightInstanceProgram>& light,
                     DynamicArray<SensorInstanceProgram>&, const EmbreeTraversalNode* node, MemoryArena& arena) override {
            light.push_back(LightInstanceProgram{ mLight, reinterpret_cast<TraversalHandle>(node), this });
        }

        AreaLightUserData* postMaterialize(ResourceHolder& holder) const {
            if(mUserData)
                holder.retain(mUserData);
            return mUserData ? &mUserData->value : nullptr;
        }
    };

    class EmbreeLeafNodeWithSensor final : public EmbreeNode {
    private:
        GeometryHandle mGeometry;
        SceneHandle mScene;
        SharedPtr<Sensor> mSensor;

    public:
        EmbreeLeafNodeWithSensor(PiperContext& context, const RTCDevice device, SharedPtr<Sensor> sensor)
            : EmbreeNode(context), mSensor(std::move(sensor)) {
            mGeometry.reset(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER));

            // dummy node
            rtcSetGeometryUserPrimitiveCount(mGeometry.get(), 0);
            rtcSetGeometryIntersectFunction(mGeometry.get(), [](auto) {});
            rtcSetGeometryBoundsFunction(
                mGeometry.get(), [](auto) {}, nullptr);
            rtcSetGeometryOccludedFunction(mGeometry.get(), [](auto) {});
            rtcCommitGeometry(mGeometry.get());

            mScene.reset(rtcNewScene(device));
            rtcAttachGeometry(mScene.get(), mGeometry.get());
            rtcCommitScene(mScene.get());
        }

        [[nodiscard]] RTCScene getScene() const noexcept override {
            return mScene.get();
        }

        [[nodiscard]] Sensor& getSensor() const noexcept {
            return *mSensor;
        }

        void collect(DynamicArray<GSMInstanceProgram>&, DynamicArray<LightInstanceProgram>&,
                     DynamicArray<SensorInstanceProgram>& sensor, const EmbreeTraversalNode* node, MemoryArena& arena) override {
            sensor.push_back(SensorInstanceProgram{ mSensor, reinterpret_cast<TraversalHandle>(node), this });
        }

        // TODO: follow
        // void postMaterialize(const TraversalHandle* follow, ResourceHolder& holder) const {}
    };

    struct EmbreeRTProgram final : RTProgram {
        LinkableProgram program;
        String symbol;
        EmbreeRTProgram(PiperContext& context, LinkableProgram prog, String sym)
            : RTProgram(context), program(std::move(prog)), symbol(std::move(sym)) {}
    };

    class EmbreeTraceLauncher final : public TraceLauncher {
    private:
        Accelerator& mAccelerator;
        KernelArgument mArg;
        KernelSymbol<KernelArgument> mKernel;
        DynamicArray<ResourceView> mResources;
        std::unique_lock<std::mutex> mLock;
        SharedPtr<EmbreeNode> mRoot;
        MemoryArena mArena;

    public:
        explicit EmbreeTraceLauncher(PiperContext& context, Accelerator& accelerator, const KernelArgument& argTemplate,
                                     KernelSymbol<KernelArgument> kernel, DynamicArray<ResourceView> resources,
                                     SharedPtr<EmbreeNode> root, std::mutex& mutex)
            : TraceLauncher(context), mAccelerator(accelerator), mArg(argTemplate),
              mKernel(std::move(kernel)), mResources{ std::move(resources) }, mLock(mutex, std::try_to_lock),
              mRoot(std::move(root)), mArena(context.getAllocator(), 128) {
            if(!mLock.owns_lock())
                context.getErrorHandler().raiseException("The pipeline is locked.", PIPER_SOURCE_LOCATION());
        }
        [[nodiscard]] Future<void> launch(const RenderRECT& rect, const Function<SBTPayload, uint32_t>& launchData,
                                          const Span<ResourceView>& resources) override {
            const auto launchSBT = launchData(static_cast<uint32_t>(mResources.size()));
            auto* ptr = reinterpret_cast<void*>(mArena.allocRaw(launchSBT.size()));
            memcpy(ptr, launchSBT.data(), launchSBT.size());
            auto arg = mArg;

            arg.rect = *reinterpret_cast<const RenderRECTAlias*>(&rect);
            arg.launchData = ptr;

            // TODO: reduce copy
            auto fullResources = mResources;
            for(auto&& res : resources)
                fullResources.push_back(res);

            return mAccelerator.launchKernel(Dim3{ rect.width, rect.height, 1 }, Dim3{ arg.sampleCount, 1, 1 }, mKernel,
                                             fullResources, arg);
        }
        [[nodiscard]] RenderRECT getRenderRECT() const noexcept override {
            return *reinterpret_cast<const RenderRECT*>(&mArg.fullRect);
        }
        void updateTimeInterval(const Time<float> begin, const Time<float> end) noexcept override {
            mRoot->updateTimeInterval(begin, end);
        }
        [[nodiscard]] Pair<uint32_t, uint32_t> getFilmResolution() const noexcept override {
            return { mArg.width, mArg.height };
        }
    };

    struct SensorGroup final {
        SensorFunc rayGen;
        const void* RGPayload;
    };

    class EmbreePipeline final : public Pipeline {
    private:
        Optional<KernelSymbol<KernelArgument>> mKernel;
        Accelerator& mAccelerator;
        // TODO:temp arena?
        MemoryArena mArena;
        KernelArgument mArg;
        ResourceHolder mHolder;
        SharedPtr<EmbreeNode> mScene;
        UMap<Node*, SensorGroup> mSensors;
        SharedPtr<Sampler> mSampler;
        EmbreeProfiler mProfiler;
        DynamicArray<ResourceView> mResources;
        std::mutex mMutex;

        void* upload(const SBTPayload& payload) {
            if(payload.empty())
                return nullptr;
            auto* ptr = reinterpret_cast<void*>(mArena.allocRaw(payload.size()));
            memcpy(ptr, payload.data(), payload.size());
            return ptr;
        }

    public:
        EmbreePipeline(PiperContext& context, Tracer& tracer, Accelerator& accelerator, ResourceCacheManager& cacheManager,
                       const PITU& pitu, SharedPtr<EmbreeNode> scene, Integrator& integrator, RenderDriver& renderDriver,
                       LightSampler& lightSampler, SharedPtr<Sampler> sampler, bool debug)
            : Pipeline(context), mAccelerator(accelerator), mArena(context.getAllocator(), 4096), mArg{}, mHolder(context),
              mScene(std::move(scene)), mSensors{ context.getAllocator() }, mSampler(std::move(sampler)),
              mProfiler(context), mResources{ context.getAllocator() } {
            DynamicArray<LinkableProgram> modules(context.getAllocator());
            modules.push_back(prepareKernelNative(context, debug));
            modules.push_back(pitu.generateLinkable(accelerator.getSupportedLinkableFormat()));

            DynamicArray<Pair<String, void*>> call(context.getAllocator());

            const MaterializeContext materialize{
                tracer,
                accelerator,
                cacheManager,
                mProfiler,
                ResourceRegister{ [this](SharedPtr<Resource> resource) {
                    const auto idx = static_cast<uint32_t>(mResources.size());
                    mResources.push_back(ResourceView{ std::move(resource), ResourceAccessMode::ReadOnly });
                    return idx;
                } },
                CallSiteRegister{ [&](const SharedPtr<RTProgram>& program, const SBTPayload& payload) -> CallHandle {
                    const auto id = static_cast<uint32_t>(call.size());
                    const auto* prog = dynamic_cast<EmbreeRTProgram*>(program.get());
                    modules.emplace_back(prog->program);
                    call.emplace_back(prog->symbol, upload(payload));
                    return reinterpret_cast<CallHandle>(static_cast<ptrdiff_t>(id));
                } },
                TextureLoader{ [&](const SharedPtr<Config>& desc, const uint32_t channel) -> CallHandle {
                    const auto texture = tracer.generateTexture(desc, channel);
                    auto [SBT, prog] = texture->materialize(materialize);
                    return materialize.registerCall(prog, SBT);
                } }
            };

            mArg.scene = mScene->getScene();

            DynamicArray<GSMInstanceProgram> GSMs{ context.getAllocator() };
            DynamicArray<LightInstanceProgram> lights{ context.getAllocator() };
            DynamicArray<SensorInstanceProgram> sensors{ context.getAllocator() };
            mScene->collect(GSMs, lights, sensors, nullptr, mArena);

            struct SurfaceInfo final {
                void* payload;
                // TODO:prefix
                String initFunc;
                String sampleFunc;
                String evaluateFunc;
                String pdfFunc;
                SurfaceInitFunc init;
                SurfaceSampleFunc sample;
                SurfaceEvaluateFunc evaluate;
                SurfacePdfFunc pdf;
            };
            UMap<const Surface*, SurfaceInfo> surfaceProg(context.getAllocator());
            struct GeometryInfo final {
                void* payload;
                String calcSurfaceFunc, intersectFunc, occludeFunc, boundsFunc;
                GeometryPostProcessFunc calcSurface;
                HitKind kind;
                EmbreeAcceleration* acceleration;
            };
            UMap<const Geometry*, GeometryInfo> geometryProg(context.getAllocator());

            const auto registerProgram = [&](const SharedPtr<RTProgram>& prog) {
                auto& rtp = dynamic_cast<EmbreeRTProgram&>(*prog);
                modules.push_back(rtp.program);
                return rtp.symbol;
            };

            const auto registerGeometry = [&](const Geometry* geometry) {
                if(geometryProg.count(geometry))
                    return;
                const auto gp = geometry->materialize(materialize);
                auto& info = geometryProg[geometry];
                auto& accel = dynamic_cast<EmbreeAcceleration&>(geometry->getAcceleration(tracer, accelerator, cacheManager));
                mResources.push_back(ResourceView{ accel.getResource(), ResourceAccessMode::ReadOnly });
                info.acceleration = &accel;

                if(gp.surface) {
                    info.kind = HitKind::Custom;
                    info.payload = upload(gp.payload);
                    info.calcSurfaceFunc = registerProgram(gp.surface);
                    info.intersectFunc = registerProgram(gp.intersect);
                    info.occludeFunc = registerProgram(gp.occlude);
                } else {
                    auto [func, payload] = accel.getBuiltin();
                    info.calcSurfaceFunc = String{ func, context.getAllocator() };
                    info.payload = upload(packSBTPayload(context.getAllocator(), payload));
                    info.kind = HitKind::Builtin;
                }
            };

            for(auto&& prog : GSMs) {
                if(!surfaceProg.count(prog.surface.get())) {
                    auto sp = prog.surface->materialize(materialize);
                    auto& info = surfaceProg[prog.surface.get()];
                    info.payload = upload(sp.payload);
                    info.initFunc = registerProgram(sp.init);
                    info.sampleFunc = registerProgram(sp.sample);
                    info.evaluateFunc = registerProgram(sp.evaluate);
                    info.pdfFunc = registerProgram(sp.pdf);
                }

                registerGeometry(prog.geometry.get());
            }

            auto ACP = renderDriver.materialize(materialize);
            auto& ACRTP = dynamic_cast<EmbreeRTProgram&>(*ACP.accumulate);
            modules.push_back(ACRTP.program);
            mArg.ACPayload = upload(ACP.payload);

            LightInstanceProgram* environmentLight = nullptr;
            for(auto&& inst : lights) {
                if(match(inst.light->attributes(), LightAttributes::Infinite)) {
                    if(environmentLight == nullptr)
                        environmentLight = &inst;
                    else
                        context.getErrorHandler().raiseException("Only one infinite light is supported.",
                                                                 PIPER_SOURCE_LOCATION());
                }
            }
            std::swap(*environmentLight, lights.front());

            mArg.lights = mArena.alloc<LightFuncGroup>(lights.size());
            DynamicArray<LightProgram> LIPs{ context.getAllocator() };
            // TODO:better interface
            DynamicArray<SharedPtr<Light>> lightReferences{ context.getAllocator() };
            for(size_t idx = 0; idx < lights.size(); ++idx) {
                auto&& inst = lights[idx];
                auto LIP = inst.light->materialize(inst.traversal, materialize);
                auto& init = dynamic_cast<EmbreeRTProgram&>(*LIP.init);
                auto& sample = dynamic_cast<EmbreeRTProgram&>(*LIP.sample);
                auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*LIP.evaluate);
                auto& pdf = dynamic_cast<EmbreeRTProgram&>(*LIP.pdf);
                modules.push_back(init.program);
                modules.push_back(sample.program);
                modules.push_back(evaluate.program);
                modules.push_back(pdf.program);
                mArg.lights[idx].LIPayload = upload(LIP.payload);
                LIPs.emplace_back(std::move(LIP));
                lightReferences.emplace_back(inst.light);

                const auto* geometry = inst.light->getGeometry();
                if(geometry)
                    registerGeometry(geometry);
            }

            lightSampler.preprocess({ lightReferences.cbegin(), lightReferences.cend() });
            auto LSP = lightSampler.materialize(materialize);
            auto& LSRTP = dynamic_cast<EmbreeRTProgram&>(*LSP.select);
            modules.push_back(LSRTP.program);
            mArg.LSPayload = upload(LSP.payload);

            DynamicArray<String> sensorFunc{ context.getAllocator() };
            sensorFunc.reserve(sensors.size());
            for(auto& sensor : sensors) {
                auto RGP = sensor.sensor->materialize(sensor.traversal, materialize);
                auto& RGRTP = dynamic_cast<EmbreeRTProgram&>(*RGP.rayGen);
                modules.push_back(RGRTP.program);
                sensorFunc.push_back(RGRTP.symbol);
                mSensors.emplace(makePair(sensor.node, SensorGroup{ nullptr, upload(RGP.payload) }));
            }

            auto TRP = integrator.materialize(materialize);
            auto& TRRTP = dynamic_cast<EmbreeRTProgram&>(*TRP.trace);
            modules.push_back(TRRTP.program);
            mArg.TRPayload = upload(TRP.payload);

            auto SAP = mSampler->materialize(materialize);
            auto& start = dynamic_cast<EmbreeRTProgram&>(*SAP.start);
            auto& generate = dynamic_cast<EmbreeRTProgram&>(*SAP.generate);
            modules.push_back(start.program);
            modules.push_back(generate.program);

            mKernel = mAccelerator.compileKernel<KernelArgument>(
                Span<LinkableProgram>{ modules.data(), modules.data() + modules.size() },
                String{ "piperMain", context.getAllocator() });

            // TODO:better interface
            auto symbolMap = mKernel->get().getSync();

            mArg.callInfo = mArena.alloc<CallInfo>(call.size());
            for(size_t idx = 0; idx < call.size(); ++idx) {
                mArg.callInfo[idx].address = reinterpret_cast<ptrdiff_t>(symbolMap->lookup(call[idx].first));
                mArg.callInfo[idx].SBTData = call[idx].second;
            }

            for(auto& [_, prog] : geometryProg) {
                prog.calcSurface = static_cast<GeometryPostProcessFunc>(symbolMap->lookup(prog.calcSurfaceFunc));
                if(prog.kind != HitKind::Builtin) {
                    prog.acceleration->setCustomFunc(prog.payload, mArena,
                                                     static_cast<GeometryIntersectFunc>(symbolMap->lookup(prog.intersectFunc)),
                                                     static_cast<GeometryOccludeFunc>(symbolMap->lookup(prog.occludeFunc)));
                }
            }
            for(auto& [_, prog] : surfaceProg) {
                prog.init = static_cast<SurfaceInitFunc>(symbolMap->lookup(prog.initFunc));
                prog.sample = static_cast<SurfaceSampleFunc>(symbolMap->lookup(prog.sampleFunc));
                prog.evaluate = static_cast<SurfaceEvaluateFunc>(symbolMap->lookup(prog.evaluateFunc));
                prog.pdf = static_cast<SurfacePdfFunc>(symbolMap->lookup(prog.pdfFunc));
            }
            for(auto& prog : GSMs) {
                auto& data = prog.node->postMaterialize(mHolder);
                auto& geo = geometryProg[prog.geometry.get()];
                data.kind = geo.kind;
                data.calcSurface = geo.calcSurface;
                data.GEPayload = geo.payload;
                data.usage = GeometryUsage::GSM;

                auto& surf = surfaceProg[prog.surface.get()];
                data.init = surf.init;
                data.sample = surf.sample;
                data.evaluate = surf.evaluate;
                data.pdf = surf.pdf;
                data.SFPayload = surf.payload;
            }

            mArg.accumulate = static_cast<RenderDriverFunc>(symbolMap->lookup(ACRTP.symbol));

            for(size_t idx = 0; idx < lights.size(); ++idx) {
                auto& LIP = LIPs[idx];
                auto& init = dynamic_cast<EmbreeRTProgram&>(*LIP.init);
                auto& sample = dynamic_cast<EmbreeRTProgram&>(*LIP.sample);
                auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*LIP.evaluate);
                auto& pdf = dynamic_cast<EmbreeRTProgram&>(*LIP.pdf);
                mArg.lights[idx].init = static_cast<LightInitFunc>(symbolMap->lookup(init.symbol));
                mArg.lights[idx].sample = static_cast<LightSampleFunc>(symbolMap->lookup(sample.symbol));
                mArg.lights[idx].evaluate = static_cast<LightEvaluateFunc>(symbolMap->lookup(evaluate.symbol));
                mArg.lights[idx].pdf = static_cast<LightPdfFunc>(symbolMap->lookup(pdf.symbol));

                if(auto* data = lights[idx].node->postMaterialize(mHolder)) {
                    auto& geo = geometryProg[lights[idx].light->getGeometry()];
                    data->kind = geo.kind;
                    data->calcSurface = geo.calcSurface;
                    data->GEPayload = geo.payload;
                    data->usage = GeometryUsage::AreaLight;
                    data->light = reinterpret_cast<LightHandle>(mArg.lights + idx);
                }
            }

            mArg.lightSample = static_cast<LightSelectFunc>(symbolMap->lookup(LSRTP.symbol));

            for(uint32_t idx = 0; idx < sensorFunc.size(); ++idx)
                mSensors[sensors[idx].node].rayGen = static_cast<SensorFunc>(symbolMap->lookup(sensorFunc[idx]));

            mArg.trace = static_cast<IntegratorFunc>(symbolMap->lookup(TRRTP.symbol));

            mArg.start = static_cast<SampleStartFunc>(symbolMap->lookup(start.symbol));
            mArg.generate = static_cast<SampleGenerateFunc>(symbolMap->lookup(generate.symbol));

            static char p1, p2, p3, p4, p5;
            mArg.profileIntersectHit = mProfiler.registerDesc("Tracer", "Intersect Hit", &p1, StatisticsType::Bool);
            mArg.profileIntersectTime = mProfiler.registerDesc("Tracer", "Intersect Time", &p2, StatisticsType::Time);
            mArg.profileOccludeHit = mProfiler.registerDesc("Tracer", "Occlude Hit", &p3, StatisticsType::Bool);
            mArg.profileOccludeTime = mProfiler.registerDesc("Tracer", "Occlude Time", &p4, StatisticsType::Time);
            mArg.profileSampleTime = mProfiler.registerDesc("Tracer", "Per Sample Time", &p5, StatisticsType::Time);
            mArg.profiler = &mProfiler;
            mArg.errorHandler = &context.getErrorHandler();
        }
        [[nodiscard]] SharedPtr<TraceLauncher> prepare(const SharedPtr<Node>& sensor, const uint32_t width, const uint32_t height,
                                                       const FitMode fitMode) override {
            const auto sensorIter = mSensors.find(sensor.get());
            if(sensorIter == mSensors.cend())
                context().getErrorHandler().raiseException("The reference node of the active sensor cannot be resolved.",
                                                           PIPER_SOURCE_LOCATION());
            const auto deviceAspectRatio =
                dynamic_cast<const EmbreeLeafNodeWithSensor*>(sensorIter->first)->getSensor().aspectRatio();

            auto [transform, rect] = calcRenderRECT(width, height, deviceAspectRatio, fitMode);

            auto arg = mArg;
            arg.rayGen = sensorIter->second.rayGen;
            arg.RGPayload = sensorIter->second.RGPayload;

            const auto attr = mSampler->generatePayload(rect.width, rect.height);
            arg.SAPayload = upload(attr.payload);
            arg.maxDimension = attr.maxDimension;

            arg.width = width;
            arg.height = height;
            arg.transform = *reinterpret_cast<const SensorNDCAffineTransformAlias*>(&transform);
            arg.sampleCount = attr.samplesPerPixel;
            arg.fullRect = *reinterpret_cast<const RenderRECTAlias*>(&rect);
            return makeSharedObject<EmbreeTraceLauncher>(context(), mAccelerator, arg, mKernel.value(), mResources, mScene,
                                                         mMutex);
        }
        String generateStatisticsReport() const override {
            return mProfiler.generateReport();
        }
    };
    */

    class OptiX final : public Tracer {
    private:
        SharedPtr<Accelerator> mAccelerator;
        ResourceCacheManager mCache;
        DynamicArray<Pair<Context*, DeviceHandle>> mDevices;
        bool mDebugMode;

        SharedPtr<Texture> generateTextureImpl(const SharedPtr<Config>& textureDesc) const {
            const auto& attr = textureDesc->viewAsObject();
            const auto id = attr.find(String{ "ClassID", context().getAllocator() });
            if(id != attr.cend())
                return context().getModuleLoader().newInstanceT<Texture>(textureDesc).getSync();
            const auto& wrap = textureDesc->at("WrapMode");
            auto str2Mode = [this](const String& mode) {
                if(mode == "Repeat")
                    return TextureWrap::Repeat;
                if(mode == "Mirror")
                    return TextureWrap::Mirror;
                context().getErrorHandler().raiseException("Unrecognized wrap mode " + mode, PIPER_SOURCE_LOCATION());
            };
            const auto mode = str2Mode(wrap->get<String>());
            const auto image = context().getModuleLoader().newInstanceT<Image>(textureDesc->at("Image")).getSync();
            // TODO: move sampler to accelerator
            // TODO: use builtin sampler
            // return mSampler->generateTexture(image, mode);
            return nullptr;
        }

    public:
        static void logCallback(uint32_t level, const CString tag, const CString message, void* data) {
            auto&& ctx = *static_cast<PiperContext*>(data);

            const auto logLevel = [level] {
                switch(level) {
                    case 1:
                        return LogLevel::Fatal;
                    case 2:
                        return LogLevel::Error;
                    case 3:
                        return LogLevel::Warning;
                    default:
                        return LogLevel::Info;
                }
            }();

            auto& logger = ctx.getLogger();
            if(logger.allow(logLevel))
                logger.record(logLevel, String{ "[OptiX]", ctx.getAllocator() } + tag + " : " + message, PIPER_SOURCE_LOCATION());
        }
        OptiX(PiperContext& context, const SharedPtr<Config>& config)
            : Tracer(context), mCache(context), mDevices{ context.getAllocator() },
              mDebugMode(config->at("DebugMode")->get<bool>()) {
            // TODO: concurrency
            mAccelerator = context.getModuleLoader().newInstanceT<Accelerator>(config->at("Accelerator")).getSync();
            // FIXME: use StringView
            if(std::string_view{ mAccelerator->getNativePlatform() } != std::string_view{ "NVIDIA CUDA" })
                context.getErrorHandler().raiseException("Unsupported accelerator", PIPER_SOURCE_LOCATION());

            for(auto ctx : mAccelerator->enumerateContexts()) {
                auto guard = ctx->makeCurrent();
                // TODO: log level
                OptixDeviceContextOptions opt{ logCallback, &context, 4,
                                               mDebugMode ? OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL :
                                                            OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_OFF };
                OptixDeviceContext device;
                checkOptixResult(context, PIPER_SOURCE_LOCATION(),
                                 optixDeviceContextCreate(reinterpret_cast<CUcontext>(ctx->getHandle()), &opt, &device));
                mDevices.push_back({ ctx, DeviceHandle{ device, DeviceDeleter{ ctx } } });
                unsigned int version;
                checkOptixResult(
                    context, PIPER_SOURCE_LOCATION(),
                    optixDeviceContextGetProperty(device, OPTIX_DEVICE_PROPERTY_RTCORE_VERSION, &version, sizeof(version)));
                auto&& logger = context.getLogger();
                if(logger.allow(LogLevel::Info))
                    logger.record(LogLevel::Info,
                                  "RT Core version: " + toString(context.getAllocator(), version / 10) + "." +
                                      toString(context.getAllocator(), version % 10),
                                  PIPER_SOURCE_LOCATION());
                // TODO: cache manage
            }

            // rtcSetDeviceMemoryMonitorFunction();
            // mSampler = context.getModuleLoader().newInstanceT<TextureSampler>(config->at("TextureSampler")).getSync();
        }
        [[nodiscard]] SharedPtr<RTProgram> buildProgram(LinkableProgram linkable, String symbol) override {
            // return makeSharedObject<OptixRTProgram>(context(), mDevices, linkable, symbol);
            return nullptr;
        }
        [[nodiscard]] SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) override {
            return makeSharedObject<OptixAcceleration>(context(), mDevices, desc);
        }
        [[nodiscard]] SharedPtr<Node> buildNode(const SharedPtr<Object>& object) override {
            return nullptr;
            /*
            // TODO: better implementation
            if(auto gsm = eastl::dynamic_shared_pointer_cast<OptixGSMInstance>(object))
                return makeSharedObject<OptixLeafNodeWithGSM>(context(), *this, *mAccelerator, mCache, mDevices, std::move(gsm));
            if(auto light = eastl::dynamic_shared_pointer_cast<Light>(object))
                return makeSharedObject<OptixLeafNodeWithLight>(context(), *this, *mAccelerator, mCache, mDevices,
                                                                std::move(light));
            if(auto sensor = eastl::dynamic_shared_pointer_cast<Sensor>(object))
                return makeSharedObject<OptixLeafNodeWithSensor>(context(), mDevices, std::move(sensor));

            context().getErrorHandler().raiseException(
                String{ "Unrecognized object ", context().getAllocator() } + typeid(*object).name(), PIPER_SOURCE_LOCATION());
                */
        }
        [[nodiscard]] SharedPtr<Node> buildNode(const DynamicArray<Pair<TransformInfo, SharedPtr<Node>>>& children) override {
            // return makeSharedObject<OptixBranchNode>(context(), mDevices, children);
            return nullptr;
        }
        [[nodiscard]] SharedPtr<GSMInstance> buildGSMInstance(SharedPtr<Geometry> geometry, SharedPtr<Surface> surface,
                                                              SharedPtr<Medium> medium) override {
            // return makeSharedObject<OptixGSMInstance>(context(), std::move(geometry), std::move(surface), std::move(medium));
            return nullptr;
        }
        [[nodiscard]] UniqueObject<Pipeline> buildPipeline(const SharedPtr<Node>& scene, Integrator& integrator,
                                                           RenderDriver& renderDriver, LightSampler& lightSampler,
                                                           SharedPtr<Sampler> sampler) override {
            /*
            return makeUniqueObject<Pipeline, OptixPipelineHolder>(
                context(), *this, *mAccelerator, mCache, *mKernel.getSync(), eastl::dynamic_shared_pointer_cast<OptixNode>(scene),
                integrator, renderDriver, lightSampler, std::move(sampler), mDebugMode);
                */
            return nullptr;
        }
        [[nodiscard]] SharedPtr<Texture> generateTexture(const SharedPtr<Config>& textureDesc,
                                                         const uint32_t channel) const override {
            /*
            auto res = generateTextureImpl(textureDesc);
            if(res->channel() != channel)
                context().getErrorHandler().raiseException("Mismatched channel. Expect " +
                                                               toString(context().getAllocator(), channel) + ", but get " +
                                                               toString(context().getAllocator(), res->channel()),
                                                           PIPER_SOURCE_LOCATION());
            return res;
            */
            return nullptr;
        }
        Accelerator& getAccelerator() const noexcept override {
            return *mAccelerator;
        }
        // ReSharper disable once CppNotAllPathsReturnValue
        [[nodiscard]] size_t getAlignmentRequirement(const AlignmentRequirement requirement) const noexcept override {
            switch(requirement) {
                case AlignmentRequirement::VertexBuffer:
                    [[fallthrough]];
                case AlignmentRequirement::IndexBuffer:
                    [[fallthrough]];
                case AlignmentRequirement::TextureCoordsBuffer:
                    return OPTIX_ACCEL_BUFFER_BYTE_ALIGNMENT;  // TODO: check alignment requirement again
                case AlignmentRequirement::BoundsBuffer:
                    return OPTIX_AABB_BUFFER_BYTE_ALIGNMENT;
            }
        }
    };
    class ModuleImpl final : public Module {
    private:
        void* mHandle;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path) : Module{ context }, mHandle{ nullptr } {
            checkOptixResult(context, PIPER_SOURCE_LOCATION(), optixInitWithHandle(&mHandle));
        }
        ~ModuleImpl() override {
            checkOptixResult(context(), PIPER_SOURCE_LOCATION(), optixUninitWithHandle(&mHandle));
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Tracer") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<OptiX>(context(), config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
