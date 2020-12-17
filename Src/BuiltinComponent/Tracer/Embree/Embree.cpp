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

#define PIPER_EXPORT
#include "../../../Interface/BuiltinComponent/Environment.hpp"
#include "../../../Interface/BuiltinComponent/Geometry.hpp"
#include "../../../Interface/BuiltinComponent/Integrator.hpp"
#include "../../../Interface/BuiltinComponent/Light.hpp"
#include "../../../Interface/BuiltinComponent/RenderDriver.hpp"
#include "../../../Interface/BuiltinComponent/Sampler.hpp"
#include "../../../Interface/BuiltinComponent/Sensor.hpp"
#include "../../../Interface/BuiltinComponent/Surface.hpp"
#include "../../../Interface/BuiltinComponent/Tracer.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#include "../../../Kernel/Protocol.hpp"
#include <random>

#include <cassert>
#include <embree3/rtcore.h>
#include <spdlog/spdlog.h>
#include <utility>

// TODO:https://www.embree.org/api.html#performance-recommendations

namespace Piper {
    // TODO:move to Kernel

    struct KernelArgument final {
        RenderRECT rect;
        RTCScene scene;
        SensorFunc rayGen;
        void* RGPayload;
        EnvironmentFunc missing;
        void* MSPayload;
        RenderDriverFunc accumulate;
        void* ACPayload;
        IntegratorFunc trace;
        void* TRPayload;
        LightFunc light;
        void* LIPayload;
        SampleFunc generate;
        void* SAPayload;
        float* samples;
        uint32_t sample;
        uint32_t maxDimension;
        SensorNDCAffineTransform transform;
    };
    enum class HitKind { Builtin, Custom };
    struct InstanceUserData final : public Object {
        PIPER_INTERFACE_CONSTRUCT(InstanceUserData, Object)
        HitKind kind;
        SurfaceEvaluateFunc evaluate;
        SurfaceSampleFunc sample;
        void* SFPayload;
        // TODO:medium
        GeometryFunc calcSurface;
        void* GEPayload;
    };

    struct IntersectContext final {
        RTCIntersectContext ctx;
        // extend information
    };
    // static void intersect() {}

    void piperTrace(FullContext* context, const RayInfo& ray, float minT, float maxT, TraceResult& result) {
        auto ctx = reinterpret_cast<KernelArgument*>(context);
        IntersectContext intersectCtx;
        rtcInitIntersectContext(&intersectCtx.ctx);
        // TODO:context flags
        intersectCtx.ctx.flags = RTC_INTERSECT_CONTEXT_FLAG_INCOHERENT;

        RTCRayHit hit{};
        hit.ray.org_x = ray.origin.x.val;
        hit.ray.org_y = ray.origin.y.val;
        hit.ray.org_z = ray.origin.z.val;
        hit.ray.tnear = minT;

        hit.ray.dir_x = ray.direction.x.val;
        hit.ray.dir_y = ray.direction.y.val;
        hit.ray.dir_z = ray.direction.z.val;
        hit.ray.time = 0.0f;  // TODO:motion blur

        hit.ray.tfar = maxT;
        hit.ray.mask = 1;  // TODO:mask

        hit.hit.geomID = hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

        // TODO:Coroutine+SIMD?
        rtcIntersect1(ctx->scene, &intersectCtx.ctx, &hit);

        if(hit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            // TODO:surface intersect filter?
            result.kind = TraceKind::Surface;
            result.surface.t = hit.ray.tfar;

            auto scene = ctx->scene;
            RTCGeometry geo = nullptr;

            for(uint32_t i = 0; i < RTC_MAX_INSTANCE_LEVEL_COUNT && hit.hit.instID[i] != RTC_INVALID_GEOMETRY_ID; ++i) {
                geo = rtcGetGeometry(scene, hit.hit.instID[i]);
                scene = static_cast<RTCScene>(rtcGetGeometryUserData(geo));
            }

            assert(geo);

            rtcGetGeometryTransform(geo, 0.0f, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, result.surface.transform.A2B);
            calcInverse(result.surface.transform.A2B, result.surface.transform.B2A);
            const auto* data = static_cast<const InstanceUserData*>(rtcGetGeometryUserData(geo));

            HitInfo hitInfo;
            if(data->kind == HitKind::Builtin) {
                hitInfo.builtin.Ng = result.surface.transform(Normal<float, FOR::World>{ Vector<Dimensionless<float>, FOR::World>{
                    Dimensionless<float>{ hit.hit.Ng_x }, Dimensionless<float>{ hit.hit.Ng_y },
                    Dimensionless<float>{ hit.hit.Ng_z } } });
                hitInfo.builtin.index = hit.hit.primID;
                hitInfo.builtin.barycentric = { hit.hit.u, hit.hit.v };
            } else {
                // TODO:custom
            }

            data->calcSurface(reinterpret_cast<RestrictedContext*>(context), data->GEPayload, hitInfo, ray.t,
                              result.surface.intersect);

            static_assert(sizeof(void*) == 8);
            result.surface.instance = reinterpret_cast<uint64_t>(data);
        } else {
            result.kind = TraceKind::Missing;
        }
    }

    void piperMissing(FullContext* context, const RayInfo& ray, Spectrum<Radiance>& radiance) {
        const auto* SBT = reinterpret_cast<KernelArgument*>(context);
        SBT->missing(decay(context), SBT->MSPayload, ray, radiance);
    }
    void piperSurfaceSample(FullContext* context, uint64_t instance, const Normal<float, FOR::Shading>& wi, float t,
                            SurfaceSample& sample) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->sample(decay(context), func->SFPayload, wi, t, sample);
    }
    void piperSurfaceEvaluate(FullContext* context, uint64_t instance, const Normal<float, FOR::Shading>& wi,
                              const Normal<float, FOR::Shading>& wo, float t, Spectrum<Dimensionless<float>>& f) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->evaluate(decay(context), func->SFPayload, wi, wo, t, f);
    }
    void piperLightSample(FullContext* context, const Point<Distance, FOR::World>& hit, float t, LightSample& sample) {
        const auto* SBT = reinterpret_cast<const KernelArgument*>(context);
        SBT->light(decay(context), SBT->LIPayload, hit, t, sample);
    }

    struct MainArgument final {
        const KernelArgument* SBT;
    };

    using RandomEngine = std::mt19937_64;

    struct PerSampleContext final {
        KernelArgument argument;
        RandomEngine eng;
        uint32_t sampleIdx;
        float* samples;
    };

    static void piperEmbreeMain(uint32_t idx, const MainArgument* arg) {
        const auto* SBT = arg->SBT;
        const auto x = SBT->rect.left + idx % SBT->rect.width;
        const auto y = SBT->rect.top + idx / SBT->rect.width;
        const auto sampleIdx = x + y * SBT->rect.width;
        PerSampleContext context{ *arg->SBT, RandomEngine{ sampleIdx * SBT->rect.height + SBT->sample }, 0,
                                  SBT->samples + sampleIdx * SBT->maxDimension * sizeof(float) };
        SBT->generate(SBT->SAPayload, x, y, SBT->sample, context.samples);
        RayInfo ray;
        Vector2<float> point;
        SBT->rayGen(reinterpret_cast<RestrictedContext*>(&context), SBT->RGPayload, x, y, SBT->rect.width, SBT->rect.height,
                    SBT->transform, ray, point);
        Spectrum<Radiance> sample;
        SBT->trace(reinterpret_cast<FullContext*>(&context), SBT->TRPayload, ray, sample);
        SBT->accumulate(reinterpret_cast<RestrictedContext*>(&context), SBT->ACPayload, point, sample);
    }

    float piperSample(RestrictedContext* context) {
        auto* ctx = reinterpret_cast<PerSampleContext*>(context);
        if(ctx->sampleIdx < ctx->argument.maxDimension)
            return ctx->samples[ctx->sampleIdx++];
        return std::generate_canonical<float, -1>(ctx->eng);
    }

    static LinkableProgram prepareKernelNative(PiperContext& context) {
        static_assert(sizeof(void*) == 8);
        DynamicArray<std::byte> res{ context.getAllocator() };
        constexpr auto header = "Native";
        res.insert(res.cend(), reinterpret_cast<const std::byte*>(header), reinterpret_cast<const std::byte*>(header + 6));
        auto append = [&res](const StringView symbol, auto address) {
            res.insert(res.cend(), reinterpret_cast<const std::byte*>(symbol.data()),
                       reinterpret_cast<const std::byte*>(symbol.data() + symbol.size() + 1));
            auto func = reinterpret_cast<uint64_t>(address);
            const auto beg = reinterpret_cast<const std::byte*>(&func);
            const auto end = beg + sizeof(func);
            res.insert(res.cend(), beg, end);
        };
#define PIPER_APPEND(FUNC) append(#FUNC, FUNC)
        PIPER_APPEND(piperTrace);
        PIPER_APPEND(piperEmbreeMain);
        PIPER_APPEND(piperLightSample);
        PIPER_APPEND(piperMissing);
        PIPER_APPEND(piperSample);
        PIPER_APPEND(piperSurfaceEvaluate);
        PIPER_APPEND(piperSurfaceSample);
#undef PIPER_APPEND
        return { res, "Native" };
    }

    struct DeviceDeleter {
        void operator()(RTCDevice device) const {
            rtcReleaseDevice(device);
        }
    };
    using DeviceHandle = UniquePtr<RTCDeviceTy, DeviceDeleter>;

    struct GeometryDeleter {
        void operator()(RTCGeometry geometry) const {
            rtcReleaseGeometry(geometry);
        }
    };
    using GeometryHandle = UniquePtr<RTCGeometryTy, GeometryDeleter>;

    struct SceneDeleter {
        void operator()(RTCScene scene) const {
            rtcReleaseScene(scene);
        }
    };
    using SceneHandle = UniquePtr<RTCSceneTy, SceneDeleter>;

    // TODO:per-vertex TBN,TexCoord
    static void calcTriangleMeshSurface(RestrictedContext*, const void*, const HitInfo& hit, float t,
                                        SurfaceIntersectionInfo& info) {
        info.N = hit.builtin.Ng;
        const Normal<float, FOR::Local> u1{ { { 1.0f }, { 0.0f }, { 0.0f } }, Unchecked{} };
        const Normal<float, FOR::Local> u2{ { { 0.0f }, { 1.0f }, { 0.0f } }, Unchecked{} };
        if(fabsf(dot(info.N, u1).val) < fabsf(dot(info.N, u2).val))
            info.T = cross(info.N, u1);
        else
            info.T = cross(info.N, u2);
        info.B = cross(info.N, info.T);
        info.texCoord = { 0.0f, 0.0f };
    }

    class EmbreeAcceleration final : public AccelerationStructure {
    private:
        GeometryHandle mGeometry;
        SceneHandle mScene;
        GeometryFunc mBuiltin;

    public:
        EmbreeAcceleration(PiperContext& context, RTCDevice device, const GeometryDesc& desc) : AccelerationStructure(context) {
            switch(desc.type) {
                case PrimitiveShapeType::TriangleIndexed: {
                    mBuiltin = calcTriangleMeshSurface;

                    mGeometry.reset(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE));
                    auto triDesc = desc.triangleIndexed;
                    const auto vert = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                                              triDesc.stride, triDesc.vertCount);
                    memcpy(vert, reinterpret_cast<void*>(triDesc.vertices), triDesc.stride * triDesc.vertCount);
                    const auto index = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                                               sizeof(uint32_t) * 3, triDesc.triCount);
                    memcpy(index, reinterpret_cast<void*>(triDesc.index), sizeof(uint32_t) * 3 * triDesc.triCount);
                    // rtcSetGeometryTimeStepCount(mGeometry.get(), 1);
                    rtcSetGeometryMask(mGeometry.get(), 1);
                    if(triDesc.transform.has_value())
                        rtcSetGeometryTransform(mGeometry.get(), 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, triDesc.transform.value().A2B);

                    rtcCommitGeometry(mGeometry.get());
                } break;
                default:
                    context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            }
        }
        [[nodiscard]] GeometryFunc getBuiltinFunc() const noexcept {
            return mBuiltin;
        }
        [[nodiscard]] RTCGeometry getGeometry() const noexcept {
            return mGeometry.get();
        }
        RTCScene getScene(RTCDevice device) {
            if(!mScene) {
                // TODO:lock+double check/call_once
                mScene.reset(rtcNewScene(device));
                // rtcSetSceneFlags(mScene.get(), RTC_SCENE_FLAG_CONTEXT_FILTER_FUNCTION);
                rtcAttachGeometry(mScene.get(), mGeometry.get());
                rtcCommitScene(mScene.get());
            }
            return mScene.get();
        }
    };

    static void attachSubScene(RTCDevice device, RTCScene dest, RTCScene src,
                               const Optional<Transform<Distance, FOR::Local, FOR::World>>& transform, void* userData) {
        const auto geo = GeometryHandle{ rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE) };
        rtcSetGeometryInstancedScene(geo.get(), src);
        // rtcSetGeometryTimeStepCount(geo.get(), 1);
        if(transform.has_value())
            rtcSetGeometryTransform(geo.get(), 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, transform.value().A2B);
        rtcSetGeometryMask(geo.get(), 1);
        rtcSetGeometryUserData(geo.get(), userData);

        rtcCommitGeometry(geo.get());
        rtcAttachGeometry(dest, geo.get());
    }

    struct InstanceProgram final {
        SharedPtr<Geometry> geometry;
        SharedPtr<Surface> surface;
    };
    class EmbreeLeafNode;
    class EmbreeNode : public Node {
    public:
        PIPER_INTERFACE_CONSTRUCT(EmbreeNode, Node)
        virtual RTCScene getScene() = 0;
        virtual void collect(UMap<EmbreeLeafNode*, InstanceProgram>& programs) = 0;
    };

    class EmbreeBranchNode final : public EmbreeNode {
    private:
        SceneHandle mScene;
        MemoryArena mArena;
        DynamicArray<SharedPtr<EmbreeNode>> mChildren;

    public:
        EmbreeBranchNode(PiperContext& context, Tracer& tracer, RTCDevice device, const DynamicArray<NodeInstanceDesc>& instances)
            : EmbreeNode(context), mArena(context.getAllocator(), 512) {
            mScene.reset(rtcNewScene(device));
            // rtcSetSceneFlags(mScene.get(), RTC_SCENE_FLAG_CONTEXT_FILTER_FUNCTION);
            for(auto&& inst : instances) {
                auto node = eastl::dynamic_shared_pointer_cast<EmbreeNode>(inst.node);
                const auto scene = node->getScene();
                attachSubScene(device, mScene.get(), scene, inst.transform, scene);
                mChildren.emplace_back(std::move(node));
            }
            rtcCommitScene(mScene.get());
        }
        RTCScene getScene() override {
            return mScene.get();
        }
        void collect(UMap<EmbreeLeafNode*, InstanceProgram>& programs) override {
            for(auto&& child : mChildren)
                child->collect(programs);
        }
    };

    class EmbreeLeafNode final : public EmbreeNode {
    private:
        SceneHandle mScene;
        GSMInstanceDesc mInstance;
        // TODO:use arena
        SharedPtr<InstanceUserData> mUserData;

    public:
        EmbreeLeafNode(PiperContext& context, Tracer& tracer, RTCDevice device, const GSMInstanceDesc& gsm)
            : EmbreeNode(context), mInstance(gsm) {
            mScene.reset(rtcNewScene(device));
            // rtcSetSceneFlags(mScene.get(), RTC_SCENE_FLAG_CONTEXT_FILTER_FUNCTION);

            auto&& accel = dynamic_cast<EmbreeAcceleration&>(mInstance.geometry->getAcceleration(tracer));
            mUserData = makeSharedObject<InstanceUserData>(context);
            attachSubScene(device, mScene.get(), accel.getScene(device), mInstance.transform, mUserData.get());

            rtcCommitScene(mScene.get());
        }
        RTCScene getScene() override {
            return mScene.get();
        }
        void collect(UMap<EmbreeLeafNode*, InstanceProgram>& programs) override {
            if(!programs.count(this))
                programs.emplace(this, InstanceProgram{ mInstance.geometry, mInstance.surface });
        }
        void postMaterialize(const InstanceUserData& data, ResourceHolder& holder) const {
            mUserData->GEPayload = data.GEPayload;
            mUserData->SFPayload = data.SFPayload;
            mUserData->calcSurface = data.calcSurface;
            mUserData->evaluate = data.evaluate;
            mUserData->sample = data.sample;
            mUserData->kind = data.kind;
            holder.retain(mUserData);
        }
    };

    struct EmbreeRTProgram final : public RTProgram {
        Future<LinkableProgram> program;
        String symbol;
        EmbreeRTProgram(PiperContext& context, Future<LinkableProgram> program, String symbol)
            : RTProgram(context), program(std::move(program)), symbol(std::move(symbol)) {}
    };

    class EmbreePipeline final : public Pipeline {
    private:
        SharedPtr<RunnableProgram> mKernel;
        Accelerator& mAccelerator;
        // TODO:temp arena?
        MemoryArena mArena;
        KernelArgument mArg;
        ResourceHolder mHolder;
        SharedPtr<EmbreeNode> mScene;

        void* upload(const SBTPayload& payload) {
            if(payload.empty())
                return nullptr;
            const auto ptr = reinterpret_cast<void*>(mArena.allocRaw(payload.size()));
            memcpy(ptr, payload.data(), payload.size());
            return ptr;
        }

    public:
        EmbreePipeline(PiperContext& context, Tracer& tracer, SharedPtr<EmbreeNode> scene, Sensor& sensor,
                       Environment& environment, Integrator& integrator, RenderDriver& renderDriver, Light& light,
                       Sampler* sampler, uint32_t width, uint32_t height)
            : Pipeline(context), mAccelerator(tracer.getAccelerator()), mArena(context.getAllocator(), 4096), mHolder(context),
              mScene(scene) {
            DynamicArray<Future<LinkableProgram>> modules(context.getAllocator());
            auto& scheduler = context.getScheduler();
            modules.push_back(scheduler.value(prepareKernelNative(context)));

            mArg.scene = scene->getScene();
            UMap<EmbreeLeafNode*, InstanceProgram> nodeProg{ context.getAllocator() };
            scene->collect(nodeProg);

            struct SurfaceInfo final {
                void* payload;
                String sampleFunc;
                String evaluateFunc;
                SurfaceSampleFunc sample;
                SurfaceEvaluateFunc evaluate;
            };
            UMap<Surface*, SurfaceInfo> surfaceProg(context.getAllocator());
            struct GeometryInfo final {
                void* payload;
                String calcSurfaceFunc;
                GeometryFunc calcSurface;
                HitKind kind;
            };
            UMap<Geometry*, GeometryInfo> geometryProg(context.getAllocator());

            for(auto&& [_, prog] : nodeProg) {
                if(!surfaceProg.count(prog.surface.get())) {
                    auto sp = prog.surface->materialize(tracer, mHolder);
                    auto& info = surfaceProg[prog.surface.get()];
                    info.payload = upload(sp.payload);
                    auto& sample = dynamic_cast<EmbreeRTProgram&>(*sp.sample);
                    auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*sp.evaluate);
                    modules.push_back(sample.program);
                    modules.push_back(evaluate.program);
                    info.sampleFunc = sample.symbol;
                    info.evaluateFunc = evaluate.symbol;
                }
                if(!geometryProg.count(prog.geometry.get())) {
                    auto gp = prog.geometry->materialize(tracer, mHolder);
                    auto& info = geometryProg[prog.geometry.get()];

                    if(gp.surface) {
                        info.kind = HitKind::Custom;
                        info.payload = upload(gp.payload);
                        auto& surface = dynamic_cast<EmbreeRTProgram&>(*gp.surface);
                        info.calcSurfaceFunc = surface.symbol;
                        modules.push_back(surface.program);
                    } else {
                        auto& accel = dynamic_cast<EmbreeAcceleration&>(prog.geometry->getAcceleration(tracer));
                        info.calcSurface = accel.getBuiltinFunc();
                        info.kind = HitKind::Builtin;
                        info.payload = nullptr;
                    }
                }
            }

            auto ACP = renderDriver.materialize(tracer, mHolder);
            auto& ACRTP = dynamic_cast<EmbreeRTProgram&>(*ACP.accumulate);
            modules.push_back(ACRTP.program);
            mArg.ACPayload = nullptr;

            auto LIP = light.materialize(tracer, mHolder);
            auto& LIRTP = dynamic_cast<EmbreeRTProgram&>(*LIP.light);
            modules.push_back(LIRTP.program);
            mArg.LIPayload = upload(LIP.payload);

            auto MSP = environment.materialize(tracer, mHolder);
            auto& MSRTP = dynamic_cast<EmbreeRTProgram&>(*MSP.missing);
            modules.push_back(MSRTP.program);
            mArg.MSPayload = upload(MSP.payload);

            auto RGP = sensor.materialize(tracer, mHolder);
            auto& RGRTP = dynamic_cast<EmbreeRTProgram&>(*RGP.rayGen);
            modules.push_back(RGRTP.program);
            mArg.RGPayload = upload(RGP.payload);

            auto TRP = integrator.materialize(tracer, mHolder);
            auto& TRRTP = dynamic_cast<EmbreeRTProgram&>(*TRP.trace);
            modules.push_back(TRRTP.program);
            mArg.TRPayload = upload(TRP.payload);

            String sampleSymbol;
            if(sampler) {
                auto SAP = sampler->materialize(tracer, mHolder);
                auto& SARTP = dynamic_cast<EmbreeRTProgram&>(*SAP.sample);
                modules.push_back(SARTP.program);
                mArg.SAPayload = upload(SAP.payload);
                mArg.maxDimension = SAP.maxDimension;
                mArg.samples = mArena.alloc<float>(width * height * SAP.maxDimension);
                sampleSymbol = SARTP.symbol;
            } else
                mArg.maxDimension = 0;

            auto& accelerator = tracer.getAccelerator();
            auto kernel =
                accelerator.compileKernel(Span<Future<LinkableProgram>>{ modules.data(), modules.data() + modules.size() },
                                          String{ "piperEmbreeMain", context.getAllocator() });
            kernel.wait();
            mKernel = kernel.get();

            for(auto& [_, prog] : geometryProg) {
                if(prog.kind != HitKind::Builtin)
                    prog.calcSurface = static_cast<GeometryFunc>(mKernel->lookup(prog.calcSurfaceFunc));
            }
            for(auto& [_, prog] : surfaceProg) {
                prog.sample = static_cast<SurfaceSampleFunc>(mKernel->lookup(prog.sampleFunc));
                prog.evaluate = static_cast<SurfaceEvaluateFunc>(mKernel->lookup(prog.evaluateFunc));
            }
            for(auto& [node, prog] : nodeProg) {
                InstanceUserData data(context);
                auto& geo = geometryProg[prog.geometry.get()];
                data.kind = geo.kind;
                data.calcSurface = geo.calcSurface;
                data.GEPayload = geo.payload;

                auto& surf = surfaceProg[prog.surface.get()];
                data.sample = surf.sample;
                data.evaluate = surf.evaluate;
                data.SFPayload = surf.payload;
                node->postMaterialize(data, mHolder);
            }

            mArg.accumulate = static_cast<RenderDriverFunc>(mKernel->lookup(ACRTP.symbol));
            mArg.light = static_cast<LightFunc>(mKernel->lookup(LIRTP.symbol));
            mArg.missing = static_cast<EnvironmentFunc>(mKernel->lookup(MSRTP.symbol));
            mArg.rayGen = static_cast<SensorFunc>(mKernel->lookup(RGRTP.symbol));
            mArg.trace = static_cast<IntegratorFunc>(mKernel->lookup(TRRTP.symbol));

            mArg.generate = (sampler ? static_cast<SampleFunc>(mKernel->lookup(sampleSymbol)) :
                                       ([](const void*, uint32_t, uint32_t, uint32_t, float*) {}));
        }
        void run(const RenderRECT& rect, const SBTPayload& renderDriverPayload, const SensorNDCAffineTransform& transform,
                 const uint32_t sample) {
            auto stage = context().getErrorHandler().enterStage("prepare payload", PIPER_SOURCE_LOCATION());
            MemoryArena arena(context().getAllocator(), 4096);
            auto buffer = mAccelerator.createBuffer(sizeof(KernelArgument), 128);
            mArg.rect = rect;
            mArg.ACPayload = upload(renderDriverPayload);
            mArg.sample = sample;
            mArg.transform = transform;
            buffer->upload(context().getScheduler().value(DataHolder{ SharedPtr<int>{}, &mArg }));

            const auto payload = mAccelerator.createPayload(InputResource{ { buffer->ref() } });

            stage.next("launch kernel", PIPER_SOURCE_LOCATION());
            const auto future =
                mAccelerator.runKernel(rect.width * rect.height, context().getScheduler().value(mKernel), payload);
            future.wait();
        }
    };

    static CString getErrorString(const RTCError ec) {
        switch(ec) {
            case RTC_ERROR_NONE:
                return "Success";
            case RTC_ERROR_INVALID_ARGUMENT:
                return "Invalid argument";
            case RTC_ERROR_INVALID_OPERATION:
                return "Invalid operation";
            case RTC_ERROR_OUT_OF_MEMORY:
                return "Out of memory";
            case RTC_ERROR_UNSUPPORTED_CPU:
                return "Unsupported CPU";
            case RTC_ERROR_CANCELLED:
                return "Cancelled";
            default:
                return "Unknown";
        }
    }

    class Embree final : public Tracer {
    private:
        SharedPtr<Accelerator> mAccelerator;
        ResourceCacheManager mCache;
        DeviceHandle mDevice;

    public:
        ResourceCacheManager& getCacheManager() override {
            return mCache;
        }
        void reportError(const RTCError ec, CString str) const {
            context().getErrorHandler().raiseException(
                "Embree Error:" + toString(context().getAllocator(), static_cast<uint32_t>(ec)) + str, PIPER_SOURCE_LOCATION());
        }
        Embree(PiperContext& context, const SharedPtr<Config>& config) : Tracer(context), mCache(context) {
            auto accelConfig = config->at("Accelerator");
            auto accel = context.getModuleLoader().newInstance(accelConfig->at("ClassID")->get<String>(), config);
            accel.wait();
            mAccelerator = eastl::dynamic_shared_pointer_cast<Accelerator>(accel.get());
            mDevice.reset(rtcNewDevice(config->at("EmbreeDeviceConfig")->get<String>().c_str()));
            if(!mDevice) {
                const auto error = rtcGetDeviceError(nullptr);
                context.getErrorHandler().raiseException("Failed to initialize Embree RTCDevice (Error Code:" +
                                                             toString(context.getAllocator(), static_cast<uint32_t>(error)) +
                                                             ") :" + getErrorString(error),
                                                         PIPER_SOURCE_LOCATION());
            }
            rtcSetDeviceErrorFunction(
                mDevice.get(),
                [](void* userPtr, const RTCError ec, CString str) { static_cast<Embree*>(userPtr)->reportError(ec, str); }, this);
            // rtcSetDeviceMemoryMonitorFunction();
        }
        SharedPtr<RTProgram> buildProgram(Future<LinkableProgram> linkable, String symbol) override {
            return makeSharedObject<EmbreeRTProgram>(context(), linkable, symbol);
        }
        SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) override {
            return makeSharedObject<EmbreeAcceleration>(context(), mDevice.get(), desc);
        }
        SharedPtr<Node> buildNode(const NodeDesc& desc) override {
            if(desc.index() == 0)
                return makeSharedObject<EmbreeBranchNode>(context(), *this, mDevice.get(),
                                                          eastl::get<DynamicArray<NodeInstanceDesc>>(desc));
            // TODO:move
            return makeSharedObject<EmbreeLeafNode>(context(), *this, mDevice.get(), eastl::get<GSMInstanceDesc>(desc));
        }

        UniqueObject<Pipeline> buildPipeline(SharedPtr<Node> scene, Sensor& sensor, Environment& environment,
                                             Integrator& integrator, RenderDriver& renderDriver, Light& light, Sampler* sampler,
                                             uint32_t width, uint32_t height) override {
            return makeUniqueObject<Pipeline, EmbreePipeline>(
                context(), *this, eastl::dynamic_shared_pointer_cast<EmbreeNode>(scene), sensor, environment, integrator,
                renderDriver, light, sampler, width, height);
        }
        Accelerator& getAccelerator() override {
            return *mAccelerator;
        }
        void trace(Pipeline& pipeline, const RenderRECT& rect, const SBTPayload& renderDriverPayload,
                   const SensorNDCAffineTransform& transform, uint32_t sample) override {
            dynamic_cast<EmbreePipeline&>(pipeline).run(rect, renderDriverPayload, transform, sample);
        }
    };
    class ModuleImpl final : public Module {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, const char*) : Module(context) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Tracer") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Embree>(context(), config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
