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

    // TODO: need makeCurrent?
    struct DeviceDeleter {
        Context* ctx;
        void operator()(const OptixDeviceContext context) const {
            checkOptixResult(ctx->context(), PIPER_SOURCE_LOCATION(), optixDeviceContextDestroy(context));
        }
    };
    using DeviceHandle = UniquePtr<OptixDeviceContext_t, DeviceDeleter>;

    struct PipelineDeleter {
        Context* ctx;
        void operator()(const OptixPipeline pipeline) const {
            checkOptixResult(ctx->context(), PIPER_SOURCE_LOCATION(), optixPipelineDestroy(pipeline));
        }
    };
    using PipelineHandle = UniquePtr<OptixPipeline_t, PipelineDeleter>;

    struct ProgramGroupDeleter {
        Context* ctx;
        void operator()(const OptixProgramGroup programGroup) const {
            checkOptixResult(ctx->context(), PIPER_SOURCE_LOCATION(), optixProgramGroupDestroy(programGroup));
        }
    };

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

            char logBuffer[8192];
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
        const SharedPtr<ResourceInstance>& requireInstance(Context* ctx) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return nullptr;
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
        SharedPtr<Resource> mResource;

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
                    mResource = triDesc.buffer;

                    // TODO: relocate
                    for(auto& [ctx, device] : devices) {
                        auto guard = ctx->makeCurrent();
                        // TODO: batch building
                        triDesc.buffer->requireInstance(ctx)->getFuture()->wait();
                        const auto bufferPtr = static_cast<CUdeviceptr>(triDesc.buffer->requireInstance(ctx)->getHandle());

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
                    mResource = custom.bounds;

                    for(auto& [ctx, device] : devices) {
                        auto guard = ctx->makeCurrent();
                        custom.bounds->requireInstance(ctx)->getFuture()->wait();
                        const auto bounds = static_cast<CUdeviceptr>(custom.bounds->requireInstance(ctx)->getHandle());

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
    };

    struct CUDATexDeleter {
        Context* ctx;
        void operator()(void* tex) const {
            auto guard = ctx->makeCurrent();
            checkCUDAResult(ctx->context(), PIPER_SOURCE_LOCATION(), cuTexObjectDestroy(reinterpret_cast<CUtexObject>(tex)));
        }
    };
    using CUDATexture = UniquePtr<void, CUDATexDeleter>;

    // TODO: multi-frame
    class BuiltinTextureInstance : public ResourceInstance {
    private:
        CUDATexture mTexture;

    public:
        BuiltinTextureInstance(PiperContext& context, SharedPtr<ResourceInstance> buffer, const ImageAttributes& attributes,
                               TextureWrap wrapMode)
            : ResourceInstance{ context } {}
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            return reinterpret_cast<ResourceHandle>(mTexture.get());
        }
    };

    class BuiltinTextureResource : public Resource {
    private:
        std::shared_mutex mMutex;
        UMap<Context*, SharedPtr<ResourceInstance>> mInstances;
        SharedPtr<Resource> mBuffer;
        TextureWrap mMode;
        ImageAttributes mAttributes;

    public:
        BuiltinTextureResource(PiperContext& context, Accelerator& accelerator, const SharedPtr<Image>& image,
                               TextureWrap wrapMode)
            : Resource{ context }, mInstances{ context.getAllocator() },
              mBuffer{ accelerator.createBuffer(
                  image->attributes().width * image->attributes().height * image->attributes().channel, 16,
                  [image, size = image->attributes().width * image->attributes().height * image->attributes().channel](
                      const Ptr ptr) { memcpy(reinterpret_cast<void*>(ptr), image->data(), size); }) },
              mMode{ wrapMode }, mAttributes{ image->attributes() } {}
        const SharedPtr<ResourceInstance>& requireInstance(Context* ctx) override {
            return safeRequireInstance(mMutex, mInstances, ctx, [this, ctx] {
                return makeSharedObject<BuiltinTextureInstance>(context(), mBuffer, mAttributes, mMode);
            });
        }
    };

    class BuiltinTexture : public Texture {
    private:
        SharedPtr<Image> mImage;
        TextureWrap mMode;

    public:
        BuiltinTexture(PiperContext& context, SharedPtr<Image> image, const TextureWrap wrapMode)
            : Texture{ context }, mImage{ std::move(image) }, mMode{ wrapMode } {}
        [[nodiscard]] uint32_t channel() const noexcept override {
            return mImage->attributes().channel;
        }
        [[nodiscard]] TextureProgram materialize(const MaterializeContext& ctx) const override {
            TextureProgram res;
            const auto tex =
                ctx.registerResource(makeSharedObject<BuiltinTextureResource>(context(), ctx.accelerator, mImage, mMode));
            res.payload = packSBTPayload(context().getAllocator(), tex);
            res.sample = nullptr;  // TODO: tex2D
            return res;
        }
    };

    // NOTICE: Concurrent launches with different values for pipelineParams in the same pipeline triggers serialization of the
    // launches. Concurrency requires a separate pipeline for each concurrent launch.

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
            auto image = context().getModuleLoader().newInstanceT<Image>(textureDesc->at("Image")).getSync();
            return makeSharedObject<BuiltinTexture>(context(), std::move(image), mode);
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
            auto res = generateTextureImpl(textureDesc);
            if(res->channel() != channel)
                context().getErrorHandler().raiseException("Mismatched channel. Expect " +
                                                               toString(context().getAllocator(), channel) + ", but get " +
                                                               toString(context().getAllocator(), res->channel()),
                                                           PIPER_SOURCE_LOCATION());
            return res;
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
        String mPath;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path)
            : Module{ context }, mHandle{ nullptr }, mPath{ path, context.getAllocator() } {
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
