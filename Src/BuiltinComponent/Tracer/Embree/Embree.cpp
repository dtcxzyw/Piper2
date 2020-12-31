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
#include "../../../Interface/BuiltinComponent/Geometry.hpp"
#include "../../../Interface/BuiltinComponent/Integrator.hpp"
#include "../../../Interface/BuiltinComponent/Light.hpp"
#include "../../../Interface/BuiltinComponent/RenderDriver.hpp"
#include "../../../Interface/BuiltinComponent/Sampler.hpp"
#include "../../../Interface/BuiltinComponent/Sensor.hpp"
#include "../../../Interface/BuiltinComponent/Surface.hpp"
#include "../../../Interface/BuiltinComponent/TextureSampler.hpp"
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

    struct LightFuncGroup final {
        LightInitFunc init;
        LightSampleFunc sample;
        LightEvaluateFunc evaluate;
        LightPdfFunc pdf;
        const void* LIPayload;
    };

    struct KernelArgument final {
        RenderRECT rect;
        RTCScene scene;
        SensorFunc rayGen;
        const void* RGPayload;
        RenderDriverFunc accumulate;
        const void* ACPayload;
        IntegratorFunc trace;
        const void* TRPayload;
        LightFuncGroup* lights;
        LightSelectFunc lightSample;
        const void* LSPayload;
        SampleFunc generate;
        const void* SAPayload;
        float* samples;
        uint32_t sample;
        uint32_t maxDimension;
        SensorNDCAffineTransform transform;
    };
    enum class HitKind { Builtin, Custom };
    struct InstanceUserData final : public Object {
        PIPER_INTERFACE_CONSTRUCT(InstanceUserData, Object)
        HitKind kind;
        SurfaceInitFunc init;
        SurfaceSampleFunc sample;
        SurfaceEvaluateFunc evaluate;
        SurfacePdfFunc pdf;

        const void* SFPayload;
        // TODO:medium
        GeometryFunc calcSurface;
        const void* GEPayload;
    };

    struct IntersectContext final {
        RTCIntersectContext ctx;
        // extend information
    };
    // static void intersect() {}

    void piperEmbreeTrace(FullContext* context, const RayInfo& ray, const float minT, const float maxT, TraceResult& result) {
        auto ctx = reinterpret_cast<KernelArgument*>(context);
        IntersectContext intersectCtx;
        rtcInitIntersectContext(&intersectCtx.ctx);
        // TODO:context flags
        intersectCtx.ctx.flags = RTC_INTERSECT_CONTEXT_FLAG_INCOHERENT;

        RTCRayHit hit{};
        hit.ray = {
            ray.origin.x.val,
            ray.origin.y.val,
            ray.origin.z.val,
            minT,
            ray.direction.x.val,
            ray.direction.y.val,
            ray.direction.z.val,
            ray.t,
            maxT,
            1,  // TODO:mask
            0,
            0  // must set the ray flags to 0
        };

        hit.hit.geomID = hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

        // TODO:Coroutine+SIMD?
        rtcIntersect1(ctx->scene, &intersectCtx.ctx, &hit);

        if(hit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            // TODO:surface intersect filter?
            result.kind = TraceKind::Surface;
            result.surface.t = Distance{ hit.ray.tfar };
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
                hitInfo.builtin.face =
                    (dot(result.surface.transform(ray.direction), hitInfo.builtin.Ng).val < 0.0f ? Face::Front : Face::Back);
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

    void piperEmbreeOcclude(FullContext* context, const RayInfo& ray, const float minT, const float maxT, bool& result) {
        const auto* ctx = reinterpret_cast<KernelArgument*>(context);
        IntersectContext intersectCtx;
        rtcInitIntersectContext(&intersectCtx.ctx);
        // TODO:context flags
        intersectCtx.ctx.flags = RTC_INTERSECT_CONTEXT_FLAG_INCOHERENT;

        RTCRay rayInfo{
            ray.origin.x.val,
            ray.origin.y.val,
            ray.origin.z.val,
            minT,
            ray.direction.x.val,
            ray.direction.y.val,
            ray.direction.z.val,
            ray.t,
            maxT,
            1,  // TODO:mask
            0,
            0  // must set the ray flags to 0
        };

        // TODO:Coroutine+SIMD?
        rtcOccluded1(ctx->scene, &intersectCtx.ctx, &rayInfo);

        result = (rayInfo.tfar < 0.0f);
    }

    void piperEmbreeSurfaceInit(FullContext* context, const uint64_t instance, const float t, const Vector2<float>& texCoord,
                                const Normal<float, FOR::Shading>& Ng, SurfaceStorage& storage, bool& noSpecular) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->init(decay(context), func->SFPayload, t, texCoord, Ng, &storage, noSpecular);
    }
    void piperEmbreeSurfaceSample(FullContext* context, const uint64_t instance, const SurfaceStorage& storage,
                                  const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& Ng, BxDFPart require,
                                  SurfaceSample& sample) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->sample(decay(context), func->SFPayload, &storage, wo, Ng, require, sample);
    }
    void piperEmbreeSurfaceEvaluate(FullContext* context, const uint64_t instance, const SurfaceStorage& storage,
                                    const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                                    const Normal<float, FOR::Shading>& Ng, BxDFPart require, Spectrum<Dimensionless<float>>& f) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->evaluate(decay(context), func->SFPayload, &storage, wo, wi, Ng, require, f);
    }
    void piperEmbreeSurfacePdf(FullContext* context, const uint64_t instance, const SurfaceStorage& storage,
                               const Normal<float, FOR::Shading>& wo, const Normal<float, FOR::Shading>& wi,
                               const Normal<float, FOR::Shading>& Ng, BxDFPart require, Dimensionless<float>& pdf) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->pdf(decay(context), func->SFPayload, &storage, wo, wi, Ng, require, pdf);
    }
    void piperEmbreeLightSelect(FullContext* context, LightSelectResult& select) {
        const auto* SBT = reinterpret_cast<const KernelArgument*>(context);
        SBT->lightSample(decay(context), SBT->LSPayload, select);
        select.light = reinterpret_cast<uint64_t>(SBT->lights + select.light);
    }
    void piperEmbreeLightInit(FullContext* context, const uint64_t light, const float t, LightStorage& storage) {
        const auto* SBT = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : SBT->lights;
        func->init(decay(context), func->LIPayload, t, &storage);
    }
    void piperEmbreeLightSample(FullContext* context, const uint64_t light, const LightStorage& storage,
                                const Point<Distance, FOR::World>& hit, LightSample& sample) {
        const auto* SBT = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : SBT->lights;
        func->sample(decay(context), func->LIPayload, &storage, hit, sample);
    }
    void piperEmbreeLightEvaluate(FullContext* context, const uint64_t light, const LightStorage& storage,
                                  const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                                  Spectrum<Radiance>& rad) {
        const auto* SBT = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : SBT->lights;
        func->evaluate(decay(context), func->LIPayload, &storage, lightSourceHit, dir, rad);
    }
    void piperEmbreeLightPdf(FullContext* context, uint64_t light, const LightStorage& storage,
                             const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                             Dimensionless<float>& pdf) {
        const auto* SBT = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : SBT->lights;
        func->pdf(decay(context), func->LIPayload, &storage, lightSourceHit, dir, pdf);
    }

    // TODO:more option
    void PIPER_CC piperEmbreePrintMessage(RestrictedContext* context, const char* msg) {
        printf("%s\n", msg);
    }
    void PIPER_CC piperEmbreePrintFloat(RestrictedContext* context, const char* desc, const float val) {
        printf("%s: %f\n", desc, val);
    }
    void PIPER_CC piperEmbreePrintUint(RestrictedContext* context, const char* desc, const uint32_t val) {
        printf("%s: %u\n", desc, val);
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

    // make compiler happy
    extern void piperMain(const uint32_t idx, const MainArgument* arg);
    static void piperEmbreeMain(const uint32_t idx, const MainArgument* arg) {
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

    float piperEmbreeSample(RestrictedContext* context) {
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
            const auto* beg = reinterpret_cast<const std::byte*>(&func);
            const auto* end = beg + sizeof(func);
            res.insert(res.cend(), beg, end);
        };
#define PIPER_APPEND(FUNC)                                                               \
    static_assert(std::is_same_v<decltype(&piper##FUNC), decltype(&piperEmbree##FUNC)>); \
    append("piper" #FUNC, piperEmbree##FUNC)
        PIPER_APPEND(Trace);
        PIPER_APPEND(Occlude);
        PIPER_APPEND(Main);
        PIPER_APPEND(LightSelect);
        PIPER_APPEND(LightInit);
        PIPER_APPEND(LightSample);
        PIPER_APPEND(LightEvaluate);
        PIPER_APPEND(LightPdf);
        PIPER_APPEND(Sample);
        PIPER_APPEND(SurfaceEvaluate);
        PIPER_APPEND(SurfaceSample);
        PIPER_APPEND(SurfaceInit);
        PIPER_APPEND(SurfacePdf);
        PIPER_APPEND(PrintMessage);
        PIPER_APPEND(PrintFloat);
        PIPER_APPEND(PrintUint);
#undef PIPER_APPEND
        return LinkableProgram{ context.getScheduler().value(res), String{ "Native", context.getAllocator() },
                                reinterpret_cast<uint64_t>(piperEmbreeMain) };
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
        info.N = info.Ng = (hit.builtin.face == Face::Front ? hit.builtin.Ng : -hit.builtin.Ng);
        const Normal<float, FOR::Local> u1{ { { 1.0f }, { 0.0f }, { 0.0f } }, Unchecked{} };
        const Normal<float, FOR::Local> u2{ { { 0.0f }, { 1.0f }, { 0.0f } }, Unchecked{} };
        if(fabsf(dot(info.N, u1).val) < fabsf(dot(info.N, u2).val))
            info.T = cross(info.N, u1);
        else
            info.T = cross(info.N, u2);
        info.B = cross(info.N, info.T);
        info.texCoord = { 0.0f, 0.0f };
    }
    static_assert(std::is_same_v<decltype(&calcTriangleMeshSurface), GeometryFunc>);

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
                    auto&& triDesc = desc.triangleIndexed;
                    auto* const vert = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                                               triDesc.stride, triDesc.vertCount);
                    memcpy(vert, reinterpret_cast<void*>(triDesc.vertices), triDesc.stride * triDesc.vertCount);
                    auto* const index = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
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
        EmbreeBranchNode(PiperContext& context, Tracer& tracer, RTCDevice device, const GroupDesc& instances)
            : EmbreeNode(context), mArena(context.getAllocator(), 512), mChildren(context.getAllocator()) {
            mScene.reset(rtcNewScene(device));
            // rtcSetSceneFlags(mScene.get(), RTC_SCENE_FLAG_CONTEXT_FILTER_FUNCTION);
            for(auto&& inst : instances.nodes) {
                auto node = eastl::dynamic_shared_pointer_cast<EmbreeNode>(inst);
                attachSubScene(device, mScene.get(), node->getScene(), instances.transform, node->getScene());
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
            mUserData->init = data.init;
            mUserData->evaluate = data.evaluate;
            mUserData->sample = data.sample;
            mUserData->pdf = data.pdf;
            mUserData->kind = data.kind;
            holder.retain(mUserData);
        }
    };

    struct EmbreeRTProgram final : public RTProgram {
        LinkableProgram program;
        String symbol;
        EmbreeRTProgram(PiperContext& context, LinkableProgram program, String symbol)
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
            auto* ptr = reinterpret_cast<void*>(mArena.allocRaw(payload.size()));
            memcpy(ptr, payload.data(), payload.size());
            return ptr;
        }

    public:
        EmbreePipeline(PiperContext& context, Tracer& tracer, SharedPtr<EmbreeNode> scene, Sensor& sensor, Integrator& integrator,
                       RenderDriver& renderDriver, const LightSampler& lightSampler, const Span<SharedPtr<Light>>& lights,
                       Sampler* sampler, uint32_t width, uint32_t height)
            : Pipeline(context), mAccelerator(tracer.getAccelerator()), mArena(context.getAllocator(), 4096), mHolder(context),
              mScene(scene) {
            DynamicArray<LinkableProgram> modules(context.getAllocator());
            auto& scheduler = context.getScheduler();
            modules.push_back(prepareKernelNative(context));

            mArg.scene = scene->getScene();
            UMap<EmbreeLeafNode*, InstanceProgram> nodeProg{ context.getAllocator() };
            scene->collect(nodeProg);

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
            UMap<Surface*, SurfaceInfo> surfaceProg(context.getAllocator());
            struct GeometryInfo final {
                void* payload;
                String calcSurfaceFunc;
                GeometryFunc calcSurface;
                HitKind kind;
            };
            UMap<Geometry*, GeometryInfo> geometryProg(context.getAllocator());

            // TODO:shared RTProgram
            for(auto&& [_, prog] : nodeProg) {
                if(!surfaceProg.count(prog.surface.get())) {
                    auto sp = prog.surface->materialize(tracer, mHolder);
                    auto& info = surfaceProg[prog.surface.get()];
                    info.payload = upload(sp.payload);
                    auto& init = dynamic_cast<EmbreeRTProgram&>(*sp.init);
                    auto& sample = dynamic_cast<EmbreeRTProgram&>(*sp.sample);
                    auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*sp.evaluate);
                    auto& pdf = dynamic_cast<EmbreeRTProgram&>(*sp.pdf);
                    modules.push_back(init.program);
                    modules.push_back(sample.program);
                    modules.push_back(evaluate.program);
                    modules.push_back(pdf.program);
                    info.initFunc = init.symbol;
                    info.sampleFunc = sample.symbol;
                    info.evaluateFunc = evaluate.symbol;
                    info.pdfFunc = pdf.symbol;
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

            auto LSP = lightSampler.materialize(tracer, mHolder);
            auto& LSRTP = dynamic_cast<EmbreeRTProgram&>(*LSP.select);
            modules.push_back(LSRTP.program);
            mArg.LSPayload = upload(LSP.payload);

            mArg.lights = mArena.alloc<LightFuncGroup>(lights.size());

            for(size_t idx = 0; idx < lights.size(); ++idx) {
                auto&& light = lights[idx];
                auto LIP = light->materialize(tracer, mHolder);
                auto& init = dynamic_cast<EmbreeRTProgram&>(*LIP.init);
                auto& sample = dynamic_cast<EmbreeRTProgram&>(*LIP.sample);
                auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*LIP.evaluate);
                auto& pdf = dynamic_cast<EmbreeRTProgram&>(*LIP.pdf);
                modules.push_back(init.program);
                modules.push_back(sample.program);
                modules.push_back(evaluate.program);
                modules.push_back(pdf.program);
                mArg.lights[idx].LIPayload = upload(LIP.payload);
            }

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
            auto kernel = accelerator.compileKernel(Span<LinkableProgram>{ modules.data(), modules.data() + modules.size() },
                                                    String{ "piperMain", context.getAllocator() });
            kernel.wait();
            mKernel = kernel.get();

            for(auto& [_, prog] : geometryProg) {
                if(prog.kind != HitKind::Builtin)
                    prog.calcSurface = static_cast<GeometryFunc>(mKernel->lookup(prog.calcSurfaceFunc));
            }
            for(auto& [_, prog] : surfaceProg) {
                prog.init = static_cast<SurfaceInitFunc>(mKernel->lookup(prog.initFunc));
                prog.sample = static_cast<SurfaceSampleFunc>(mKernel->lookup(prog.sampleFunc));
                prog.evaluate = static_cast<SurfaceEvaluateFunc>(mKernel->lookup(prog.evaluateFunc));
                prog.pdf = static_cast<SurfacePdfFunc>(mKernel->lookup(prog.pdfFunc));
            }
            for(auto& [node, prog] : nodeProg) {
                InstanceUserData data(context);
                auto& geo = geometryProg[prog.geometry.get()];
                data.kind = geo.kind;
                data.calcSurface = geo.calcSurface;
                data.GEPayload = geo.payload;

                auto& surf = surfaceProg[prog.surface.get()];
                data.init = surf.init;
                data.sample = surf.sample;
                data.evaluate = surf.evaluate;
                data.pdf = surf.pdf;
                data.SFPayload = surf.payload;
                node->postMaterialize(data, mHolder);
            }

            mArg.accumulate = static_cast<RenderDriverFunc>(mKernel->lookup(ACRTP.symbol));

            for(size_t idx = 0; idx < lights.size(); ++idx) {
                auto&& light = lights[idx];
                auto LIP = light->materialize(tracer, mHolder);
                auto& init = dynamic_cast<EmbreeRTProgram&>(*LIP.init);
                auto& sample = dynamic_cast<EmbreeRTProgram&>(*LIP.sample);
                auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*LIP.evaluate);
                auto& pdf = dynamic_cast<EmbreeRTProgram&>(*LIP.pdf);
                mArg.lights[idx].init = static_cast<LightInitFunc>(mKernel->lookup(init.symbol));
                mArg.lights[idx].sample = static_cast<LightSampleFunc>(mKernel->lookup(sample.symbol));
                mArg.lights[idx].evaluate = static_cast<LightEvaluateFunc>(mKernel->lookup(evaluate.symbol));
                mArg.lights[idx].pdf = static_cast<LightPdfFunc>(mKernel->lookup(pdf.symbol));
            }

            mArg.lightSample = static_cast<LightSelectFunc>(mKernel->lookup(LSRTP.symbol));
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
        SharedPtr<TextureSampler> mSampler;

    public:
        ResourceCacheManager& getCacheManager() override {
            return mCache;
        }
        void reportError(const RTCError ec, CString str) const {
            context().getErrorHandler().raiseException(
                "Embree Error:" + toString(context().getAllocator(), static_cast<uint32_t>(ec)) + str, PIPER_SOURCE_LOCATION());
        }
        Embree(PiperContext& context, const SharedPtr<Config>& config) : Tracer(context), mCache(context) {
            auto& accelConfig = config->at("Accelerator");
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
            auto& samplerConfig = config->at("TextureSampler");
            mSampler = context.getModuleLoader()
                           .newInstanceT<TextureSampler>(samplerConfig->at("ClassID")->get<String>(), samplerConfig)
                           .getSync();
        }
        SharedPtr<RTProgram> buildProgram(LinkableProgram linkable, String symbol) override {
            return makeSharedObject<EmbreeRTProgram>(context(), linkable, symbol);
        }
        SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) override {
            return makeSharedObject<EmbreeAcceleration>(context(), mDevice.get(), desc);
        }
        SharedPtr<Node> buildNode(const NodeDesc& desc) override {
            if(desc.index() == 0)
                return makeSharedObject<EmbreeBranchNode>(context(), *this, mDevice.get(), eastl::get<GroupDesc>(desc));
            // TODO:move
            return makeSharedObject<EmbreeLeafNode>(context(), *this, mDevice.get(), eastl::get<GSMInstanceDesc>(desc));
        }
        UniqueObject<Pipeline> buildPipeline(SharedPtr<Node> scene, Sensor& sensor, Integrator& integrator,
                                             RenderDriver& renderDriver, const LightSampler& lightSampler,
                                             const Span<SharedPtr<Light>>& lights, Sampler* sampler, uint32_t width,
                                             uint32_t height) override {
            return makeUniqueObject<Pipeline, EmbreePipeline>(
                context(), *this, eastl::dynamic_shared_pointer_cast<EmbreeNode>(scene), sensor, integrator, renderDriver,
                lightSampler, lights, sampler, width, height);
        }
        Accelerator& getAccelerator() override {
            return *mAccelerator;
        }
        void trace(Pipeline& pipeline, const RenderRECT& rect, const SBTPayload& renderDriverPayload,
                   const SensorNDCAffineTransform& transform, uint32_t sample) override {
            dynamic_cast<EmbreePipeline&>(pipeline).run(rect, renderDriverPayload, transform, sample);
        }
        SharedPtr<Texture> generateTexture(const SharedPtr<Image>& image, TextureWrap wrap) const override {
            return mSampler->generateTexture(image, wrap);
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
