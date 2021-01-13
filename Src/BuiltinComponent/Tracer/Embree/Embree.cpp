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

#include <cassert>
#include <embree3/rtcore.h>
#include <random>
#include <spdlog/spdlog.h>
#include <utility>

// TODO:https://www.embree.org/api.html#performance-recommendations

namespace Piper {
    // TODO:controlled by RenderDriver for per tile analysis
    class EmbreeProfiler final : public Profiler {
    private:
        using BoolCounter = std::pair<std::atomic_uint64_t, std::atomic_uint64_t>;
        using FloatCounter = std::pair<std::atomic<double>, std::atomic_uint64_t>;
        using UIntCounter = DynamicArray<std::atomic_uint64_t>;
        using TimeCounter = std::pair<std::atomic_uint64_t, std::atomic_uint64_t>;
        struct Record final {
            StatisticsType type;
            union {
                BoolCounter bc;
                FloatCounter fc;
                UIntCounter uc;
                TimeCounter tc;
            };
            Record(Record&& rhs) noexcept : type(rhs.type) {
                switch(type) {
                    case StatisticsType::Bool: {
                        bc.first = 0;
                        bc.second = 0;
                    } break;
                    case StatisticsType::Float: {
                        fc.first = 0.0;
                        fc.second = 0;
                    } break;
                    case StatisticsType::UInt: {
                        new(&uc) UIntCounter{ std::move(rhs.uc) };
                    } break;
                    case StatisticsType::Time: {
                        tc.first = 0;
                        tc.second = 0;
                    } break;
                }
            }

            ~Record() {
                if(type == StatisticsType::UInt)
                    std::destroy_at(&uc);
            }
        };

        DynamicArray<Record> mStatistics;
        struct ItemInfo final {
            String group;
            String name;
            StatisticsHandle id;
        };
        UMap<const void*, ItemInfo> mID;
        mutable std::mutex mMutex;
        using Clock = std::chrono::high_resolution_clock;

    public:
        explicit EmbreeProfiler(PiperContext& context)
            : Profiler(context), mStatistics(context.getAllocator()), mID(context.getAllocator()) {}
        StatisticsHandle registerDesc(const StringView group, const StringView name, const void* uid, const StatisticsType type,
                                      const uint32_t maxValue = 0) override {
            std::lock_guard<std::mutex> guard{ mMutex };
            const auto iter = mID.find(uid);
            if(iter != mID.cend())
                return iter->second.id;
            String sGroup{ group, context().getAllocator() }, sName{ name, context().getAllocator() };
            const auto* const res = reinterpret_cast<StatisticsHandle>(static_cast<ptrdiff_t>(mStatistics.size()));
            mID.insert(makePair(uid, ItemInfo{ std::move(sGroup), std::move(sName), res }));
            auto& record = *static_cast<Record*>(mStatistics.push_back_uninitialized());
            record.type = type;
            switch(type) {
                case StatisticsType::Bool: {
                    record.bc.first = 0;
                    record.bc.second = 0;
                } break;
                case StatisticsType::UInt: {
                    new(&record.uc) UIntCounter{ maxValue + 1, context().getAllocator() };
                } break;
                case StatisticsType::Float: {
                    record.fc.first = 0.0;
                    record.fc.second = 0;
                } break;
                case StatisticsType::Time: {
                    record.tc.first = 0;
                    record.tc.second = 0;
                } break;
            }
            return res;
        }
        void addBool(const StatisticsHandle id, const bool val) {
            auto& [first, second] = mStatistics[reinterpret_cast<ptrdiff_t>(id)].bc;
            ++(val ? first : second);
        }
        void addFloat(const StatisticsHandle id, const float val) {
            auto& [first, second] = mStatistics[reinterpret_cast<ptrdiff_t>(id)].fc;
            double src = first;
            while(!first.compare_exchange_strong(src, src + static_cast<double>(val)))
                ;
            ++second;
        }
        void addTime(const StatisticsHandle id, const uint64_t val) {
            auto& [first, second] = mStatistics[reinterpret_cast<ptrdiff_t>(id)].tc;
            first += val;
            ++second;
        }
        [[nodiscard]] static uint64_t getTime() {
            return Clock::now().time_since_epoch().count();
        }
        void addUInt(const StatisticsHandle id, const uint32_t val) {
            ++mStatistics[reinterpret_cast<ptrdiff_t>(id)].uc[val];
        }
        [[nodiscard]] String generateReport() const override {
            std::lock_guard<std::mutex> guard{ mMutex };
            DynamicArray<String> report{ context().getAllocator() };
            report.reserve(mStatistics.size());
            for(auto&& item : mStatistics) {
                switch(item.type) {
                    case StatisticsType::Bool: {
                        const auto& [hit, miss] = item.bc;
                        const auto tot = hit + miss;
                        report.emplace_back(
                            toString(context().getAllocator(),
                                     static_cast<double>(hit) / static_cast<double>(std::max(1ULL, tot)) * 100.0) +
                            "% (" + toString(context().getAllocator(), hit) + "/" + toString(context().getAllocator(), tot) +
                            ")\n");
                    } break;
                    case StatisticsType::Float: {
                        const auto& [sum, cnt] = item.fc;
                        report.emplace_back(
                            toString(context().getAllocator(),
                                     static_cast<double>(sum) / static_cast<double>(std::max(1ULL, static_cast<uint64_t>(cnt)))) +
                            " (" + toString(context().getAllocator(), cnt) + " samples)\n");
                    } break;
                    case StatisticsType::UInt: {
                        const auto& record = item.uc;
                        auto sum = 0.0;
                        uint64_t tot = 0;
                        String res{ context().getAllocator() };
                        for(uint32_t idx = 0; idx < record.size(); ++idx) {
                            const uint64_t cnt = record[idx];
                            sum += static_cast<double>(idx) * static_cast<double>(cnt);
                            tot += cnt;
                        }
                        for(uint32_t idx = 0; idx < record.size(); ++idx) {
                            const uint64_t cnt = record[idx];
                            res += toString(context().getAllocator(), idx) + ": " +
                                toString(context().getAllocator(),
                                         static_cast<double>(cnt) / static_cast<double>(std::max(1ULL, tot)) * 100.0) +
                                "% (" + toString(context().getAllocator(), cnt) + " samples)\n";
                        }
                        report.emplace_back("mean " +
                                            toString(context().getAllocator(), sum / static_cast<double>(std::max(1ULL, tot))) +
                                            " (" + toString(context().getAllocator(), tot) + " samples)\n" + res);
                    } break;
                    case StatisticsType::Time: {
                        using Ratio = std::ratio_divide<std::nano, Clock::period>;
                        const auto& [sum, cnt] = item.tc;
                        report.emplace_back(
                            toString(context().getAllocator(), sum * Ratio::num / std::max(1ULL, Ratio::den * cnt)) + " ns (" +
                            toString(context().getAllocator(), cnt) + " samples)\n");
                    } break;
                }
            }

            Map<String, Map<String, String>> remap(context().getAllocator());
            auto locate = [&, this](const String& group) -> Map<String, String>& {
                const auto iter = remap.find(group);
                if(iter != remap.cend())
                    return iter->second;
                return remap.emplace(group, Map<String, String>{ context().getAllocator() }).first->second;
            };
            for(auto&& [_, info] : mID)
                locate(info.group).emplace(info.name, info.name + ": " + report[reinterpret_cast<ptrdiff_t>(info.id)]);
            const auto* const line = "\n========================================\n";
            String res{ "\n=============== Pipeline Statistics ===============\n", context().getAllocator() };
            for(auto&& [group, map] : remap) {
                res += "\n=============== " + group + " ===============\n";
                for(auto&& [_, val] : map)
                    res += val;
            }
            res += line;
            return res;
        }
    };

    static void piperEmbreeStatisticsUInt(const RestrictedContext context, const StatisticsHandle statistics,
                                          const uint32_t val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addUInt(statistics, val);
    }
    static void piperEmbreeStatisticsBool(const RestrictedContext context, const StatisticsHandle statistics, const bool val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addBool(statistics, val);
    }
    static void piperEmbreeStatisticsFloat(const RestrictedContext context, const StatisticsHandle statistics, const float val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addFloat(statistics, val);
    }
    static void piperEmbreeStatisticsTime(const RestrictedContext context, const StatisticsHandle statistics,
                                          const uint64_t val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addTime(statistics, val);
    }
    static void piperEmbreeGetTime(RestrictedContext, uint64_t& val) {
        val = EmbreeProfiler::getTime();
    }

    constexpr auto gsmMask = 1, areaLightMask = 2;

    struct IntersectContext final {
        RTCIntersectContext ctx;
        // extend information
        RestrictedContext context;
        GeometryStorage* storage;
    };

    static void piperEmbreeTrace(const FullContext context, const RayInfo<FOR::World>& ray, const float minT, const float maxT,
                                 TraceResult& result) {
        const auto* ctx = reinterpret_cast<const PerSampleContext*>(context);
        const auto begin = EmbreeProfiler::getTime();

        GeometryStorage storage;
        IntersectContext intersectCtx;
        // TODO:store time of multi rays by ray id
        rtcInitIntersectContext(&intersectCtx.ctx);
        intersectCtx.context = decay(context);
        intersectCtx.storage = &storage;
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
            ctx->time.val,
            maxT,
            gsmMask | areaLightMask,
            0,
            0  // must set the ray flags to 0
        };

        hit.hit.geomID = hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

        // TODO:Coroutine+SIMD?
        rtcIntersect1(ctx->argument.scene, reinterpret_cast<RTCIntersectContext*>(&intersectCtx), &hit);

        if(hit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
            // TODO:surface intersect filter?
            result.surface.t = Distance{ hit.ray.tfar };
            auto* scene = ctx->argument.scene;
            RTCGeometry geo = nullptr;
            auto initialized = false;

            for(uint32_t i = 0; i < RTC_MAX_INSTANCE_LEVEL_COUNT && hit.hit.instID[i] != RTC_INVALID_GEOMETRY_ID; ++i) {
                geo = rtcGetGeometry(scene, hit.hit.instID[i]);

                rtcGetGeometryTransform(geo, 0.0f, RTC_FORMAT_FLOAT3X4_ROW_MAJOR,
                                        initialized ? result.surface.transform.A2B :
                                                      result.surface.transform.B2A);  // local to world
                if(initialized)
                    mergeL(result.surface.transform.B2A, result.surface.transform.A2B);
                else
                    initialized = true;

                scene = static_cast<RTCScene>(rtcGetGeometryUserData(geo));
            }

            assert(initialized);

            calcInverse(result.surface.transform.B2A, result.surface.transform.A2B);

            const auto* data = static_cast<const GeometryUserData*>(rtcGetGeometryUserData(geo));

            if(data->kind == HitKind::Builtin) {
                auto& builtin = *reinterpret_cast<BuiltinHitInfo*>(&storage);
                builtin.Ng = result.surface.transform(Normal<float, FOR::World>{ Vector<Dimensionless<float>, FOR::World>{
                    Dimensionless<float>{ hit.hit.Ng_x }, Dimensionless<float>{ hit.hit.Ng_y },
                    Dimensionless<float>{ hit.hit.Ng_z } } });
                builtin.index = hit.hit.primID;
                builtin.barycentric = { hit.hit.u, hit.hit.v };
                builtin.face = (dot(result.surface.transform(ray.direction), builtin.Ng).val < 0.0f ? Face::Front : Face::Back);
            }

            data->calcSurface(reinterpret_cast<RestrictedContext>(context), data->GEPayload, &storage, result.surface.intersect);

            if(data->usage == GeometryUsage::GSM) {
                result.kind = TraceKind::Surface;
                result.surface.surface = reinterpret_cast<SurfaceHandle>(data);
            } else {
                result.kind = TraceKind::AreaLight;
                result.surface.light = reinterpret_cast<const AreaLightUserData*>(data)->light;
            }
            ctx->argument.profiler->addBool(ctx->argument.profileIntersectHit, true);
        } else {
            result.kind = TraceKind::Missing;
            ctx->argument.profiler->addBool(ctx->argument.profileIntersectHit, false);
        }
        const auto end = EmbreeProfiler::getTime();
        ctx->argument.profiler->addTime(ctx->argument.profileIntersectTime, end - begin);
    }

    static bool piperEmbreeOcclude(const FullContext context, const RayInfo<FOR::World>& ray, const float minT,
                                   const float maxT) {
        const auto* ctx = reinterpret_cast<const PerSampleContext*>(context);
        const auto begin = EmbreeProfiler::getTime();
        IntersectContext intersectCtx;
        // TODO:store time of multi rays by ray id
        rtcInitIntersectContext(&intersectCtx.ctx);
        intersectCtx.context = decay(context);
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
            ctx->time.val,
            maxT,
            gsmMask,
            0,
            0  // must set the ray flags to 0
        };

        // TODO:Coroutine+SIMD?
        rtcOccluded1(ctx->argument.scene, reinterpret_cast<RTCIntersectContext*>(&intersectCtx), &rayInfo);

        const auto result = (rayInfo.tfar < 0.0f);
        ctx->argument.profiler->addBool(ctx->argument.profileOccludeHit, result);
        const auto end = EmbreeProfiler::getTime();
        ctx->argument.profiler->addTime(ctx->argument.profileOccludeTime, end - begin);
        return result;
    }

    static float piperEmbreeSample(const FullContext context) {
        auto* ctx = reinterpret_cast<PerSampleContext*>(context);
        if(ctx->currentDimension < ctx->argument.maxDimension) {
            float res;
            ctx->argument.generate(ctx->argument.SAPayload, ctx->sampleIndex, ctx->currentDimension++, res);
            return res;
        }
        return std::generate_canonical<float, -1>(ctx->eng);
    }

    struct EmbreeTraversalNode final {
        const EmbreeTraversalNode* parent;
        RTCGeometry geometry;
    };

    static void piperEmbreeQueryTransform(const RestrictedContext context, const TraversalHandle traversal,
                                          Transform<Distance, FOR::Local, FOR::World>& transform) {
        auto initialized = false;
        const auto t = reinterpret_cast<PerSampleContext*>(context)->time.val;
        const auto* node = reinterpret_cast<const EmbreeTraversalNode*>(traversal);
        do {
            rtcGetGeometryTransform(node->geometry, t, RTC_FORMAT_FLOAT3X4_ROW_MAJOR,
                                    initialized ? transform.B2A : transform.A2B);  // local to world
            if(initialized)
                mergeR(transform.B2A, transform.A2B);
            else
                initialized = true;
            node = node->parent;
        } while(node);
        calcInverse(transform.A2B, transform.B2A);
    }

    // TODO:more option
    static void piperEmbreePrintFloat(RestrictedContext, const char* msg, const float ref) {
        printf("%s:%lf\n", msg, static_cast<double>(ref));
    }

    static void piperEmbreeFloatAtomicAdd(float& x, const float y) {
        // TODO:improve performance
        auto& atomicX = *reinterpret_cast<std::atomic<float>*>(&x);
        auto src = x;
        while(!atomicX.compare_exchange_strong(src, src + y))
            ;
    }

    struct MainArgument final {
        const KernelArgument* SBT;
    };

    // make compiler happy
    extern void piperMain(uint32_t idx, const MainArgument* arg);
    static void piperEmbreeMain(const uint32_t idx, const MainArgument* arg) {
        const auto* SBT = arg->SBT;
        const auto x = SBT->rect.left + idx % SBT->rect.width;
        const auto y = SBT->rect.top + idx / SBT->rect.width;

        PerSampleContext context{
            *arg->SBT, { 0.0f }, 2, 0, RandomEngine{ (x + y * SBT->rect.width) * SBT->rect.height + SBT->sample }
        };
        Vector2<float> point;
        SBT->start(SBT->SAPayload, x, y, SBT->sample, context.sampleIndex, point);
        // TODO:motion blur
        // context.time={};

        const auto& transform = SBT->transform;
        const auto NDC = Vector2<float>{ transform.ox + transform.sx * point.x / static_cast<float>(SBT->rect.width),
                                         transform.oy + transform.sy * point.y / static_cast<float>(SBT->rect.height) };
        RayInfo<FOR::World> ray;
        Dimensionless<float> weight;
        SBT->rayGen(reinterpret_cast<RestrictedContext>(&context), SBT->RGPayload, NDC,
                    piperEmbreeSample(reinterpret_cast<FullContext>(&context)),
                    piperEmbreeSample(reinterpret_cast<FullContext>(&context)), ray, weight);

        Spectrum<Radiance> sample;
        SBT->trace(reinterpret_cast<FullContext>(&context), SBT->TRPayload, ray, sample);
        SBT->accumulate(reinterpret_cast<RestrictedContext>(&context), SBT->ACPayload, SBT->launchData, point, sample * weight);
    }

    static LinkableProgram prepareKernelNative(PiperContext& context, const bool debug) {
        DynamicArray<std::byte> res{ context.getAllocator() };
        constexpr auto header = "Native";
        res.insert(res.cend(), reinterpret_cast<const std::byte*>(header), reinterpret_cast<const std::byte*>(header + 6));
        auto append = [&res](const StringView symbol, auto address) {
            res.insert(res.cend(), reinterpret_cast<const std::byte*>(symbol.data()),
                       reinterpret_cast<const std::byte*>(symbol.data() + symbol.size() + 1));
            auto func = reinterpret_cast<ptrdiff_t>(address);
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
        PIPER_APPEND(Sample);
        PIPER_APPEND(StatisticsUInt);
        PIPER_APPEND(StatisticsBool);
        PIPER_APPEND(StatisticsFloat);
        PIPER_APPEND(StatisticsTime);
        PIPER_APPEND(GetTime);
        PIPER_APPEND(QueryTransform);
        PIPER_APPEND(FloatAtomicAdd);
        if(debug) {
            PIPER_APPEND(PrintFloat);
        }
#undef PIPER_APPEND
        return LinkableProgram{ context.getScheduler().value(res), String{ "Native", context.getAllocator() },
                                reinterpret_cast<uint64_t>(piperEmbreeMain) };
    }

    struct DeviceDeleter {
        void operator()(const RTCDevice device) const {
            rtcReleaseDevice(device);
        }
    };
    using DeviceHandle = UniquePtr<RTCDeviceTy, DeviceDeleter>;

    struct GeometryDeleter {
        void operator()(const RTCGeometry geometry) const {
            rtcReleaseGeometry(geometry);
        }
    };
    using GeometryHandle = UniquePtr<RTCGeometryTy, GeometryDeleter>;

    struct SceneDeleter {
        void operator()(const RTCScene scene) const {
            rtcReleaseScene(scene);
        }
    };
    using SceneHandle = UniquePtr<RTCSceneTy, SceneDeleter>;

    struct CustomBuffer final {
        const void* payload;
        GeometryIntersectFunc intersect;
        GeometryOccludeFunc occlude;
    };

    // TODO:sub class
    class EmbreeAcceleration final : public AccelerationStructure {
    private:
        GeometryHandle mGeometry;
        SceneHandle mScene;
        BuiltinTriangleBuffer mBuffer;
        CustomBuffer mCustomBuffer;

    public:
        EmbreeAcceleration(PiperContext& context, RTCDevice device, const GeometryDesc& desc)
            : AccelerationStructure(context), mBuffer{} {
            switch(desc.type) {
                case PrimitiveShapeType::TriangleIndexed: {
                    mGeometry.reset(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE));
                    auto&& triDesc = desc.triangleIndexed;
                    auto* const vert = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                                               3 * sizeof(float), triDesc.vertCount);
                    memcpy(vert, reinterpret_cast<void*>(triDesc.vertices), 3 * sizeof(float) * triDesc.vertCount);
                    auto* const index = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                                                sizeof(uint32_t) * 3, triDesc.triCount);
                    memcpy(index, reinterpret_cast<void*>(triDesc.index), sizeof(uint32_t) * 3 * triDesc.triCount);
                    mBuffer.index = static_cast<const uint32_t*>(index);

                    // rtcSetGeometryTimeStepCount(mGeometry.get(), 1);

                    rtcSetGeometryVertexAttributeCount(
                        mGeometry.get(), (triDesc.texCoords ? 1 : 0) + (triDesc.normal ? 1 : 0) + (triDesc.tangent ? 1 : 0));
                    uint32_t slot = 0;
                    // TODO: rtcSetGeometryVertexAttributeTopology
                    if(triDesc.texCoords) {
                        auto* const texCoord = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, slot++,
                                                                       RTC_FORMAT_FLOAT2, 2 * sizeof(float), triDesc.vertCount);
                        memcpy(texCoord, reinterpret_cast<void*>(triDesc.texCoords), 2 * sizeof(float) * triDesc.vertCount);
                        mBuffer.texCoord = static_cast<const Vector2<float>*>(texCoord);
                    }

                    if(triDesc.normal) {
                        auto* const normal = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, slot++,
                                                                     RTC_FORMAT_FLOAT3, 3 * sizeof(float), triDesc.vertCount);
                        memcpy(normal, reinterpret_cast<void*>(triDesc.normal), 3 * sizeof(float) * triDesc.vertCount);
                        mBuffer.texCoord = static_cast<const Vector2<float>*>(normal);
                    }

                    if(triDesc.tangent) {
                        auto* const tangent = rtcSetNewGeometryBuffer(mGeometry.get(), RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, slot,
                                                                      RTC_FORMAT_FLOAT3, 3 * sizeof(float), triDesc.vertCount);
                        memcpy(tangent, reinterpret_cast<void*>(triDesc.tangent), 3 * sizeof(float) * triDesc.vertCount);
                        mBuffer.texCoord = static_cast<const Vector2<float>*>(tangent);
                    }
                } break;
                case PrimitiveShapeType::Custom: {
                    mGeometry.reset(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER));
                    const auto& custom = desc.custom;
                    rtcSetGeometryUserPrimitiveCount(mGeometry.get(), custom.count);
                    rtcSetGeometryUserData(mGeometry.get(), reinterpret_cast<void*>(custom.bounds));  // for acceleration build
                    rtcSetGeometryBoundsFunction(
                        mGeometry.get(),
                        [](const RTCBoundsFunctionArguments* args) {
                            const auto* const bounds = static_cast<const float*>(args->geometryUserPtr) + args->primID * 6;
                            memcpy(&args->bounds_o->lower_x, bounds, 3 * sizeof(float));
                            memcpy(&args->bounds_o->upper_x, bounds + 3, 3 * sizeof(float));
                        },
                        nullptr);  // NOTICE:The userPtr is not used.
                    rtcSetGeometryIntersectFunction(mGeometry.get(), [](const RTCIntersectFunctionNArguments* args) {
                        const auto* const sbt = static_cast<const CustomBuffer*>(args->geometryUserPtr);
                        // TODO:allow SIMD?
                        auto* rayInfo = RTCRayHitN_RayN(args->rayhit, args->N);
                        auto* hitInfo = RTCRayHitN_HitN(args->rayhit, args->N);
                        for(uint32_t i = 0; i < args->N; ++i) {
                            if(args->valid[i] != -1)
                                continue;
                            RayInfo<FOR::Local> ray{
                                Point<Distance, FOR::Local>{ { RTCRayN_org_x(rayInfo, args->N, i) },
                                                             { RTCRayN_org_y(rayInfo, args->N, i) },
                                                             { RTCRayN_org_z(rayInfo, args->N, i) } },
                                Normal<float, FOR::Local>{ Vector<Dimensionless<float>, FOR::Local>{
                                    { RTCRayN_dir_x(rayInfo, args->N, i) },
                                    { RTCRayN_dir_y(rayInfo, args->N, i) },
                                    { RTCRayN_dir_z(rayInfo, args->N, i) } } },  // TODO:Unsafe?
                            };

                            auto& tFar = RTCRayN_tfar(rayInfo, args->N, i);
                            const auto oldTime = tFar;
                            sbt->intersect(reinterpret_cast<IntersectContext*>(args->context)->context, sbt->payload,
                                           args->primID, ray, RTCRayN_tnear(rayInfo, args->N, i), tFar,
                                           reinterpret_cast<IntersectContext*>(args->context)->storage);
                            if(tFar < oldTime) {
                                RTCHitN_geomID(hitInfo, args->N, i) = args->geomID;
                                for(uint32_t l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l)
                                    RTCHitN_instID(hitInfo, args->N, i, l) = args->context->instID[l];
                            }
                        }
                    });
                    rtcSetGeometryOccludedFunction(mGeometry.get(), [](const RTCOccludedFunctionNArguments* args) {
                        const auto* const sbt = static_cast<const CustomBuffer*>(args->geometryUserPtr);
                        // TODO:allow SIMD?
                        for(uint32_t i = 0; i < args->N; ++i) {
                            if(args->valid[i] != -1)
                                continue;
                            RayInfo<FOR::Local> ray{
                                Point<Distance, FOR::Local>{ { RTCRayN_org_x(args->ray, args->N, i) },
                                                             { RTCRayN_org_y(args->ray, args->N, i) },
                                                             { RTCRayN_org_z(args->ray, args->N, i) } },
                                Normal<float, FOR::Local>{ Vector<Dimensionless<float>, FOR::Local>{
                                    { RTCRayN_dir_x(args->ray, args->N, i) },
                                    { RTCRayN_dir_y(args->ray, args->N, i) },
                                    { RTCRayN_dir_z(args->ray, args->N, i) } } },  // TODO:Unsafe?
                            };

                            auto& tFar = RTCRayN_tfar(args->ray, args->N, i);
                            bool hit;
                            sbt->occlude(reinterpret_cast<IntersectContext*>(args->context)->context, sbt->payload, args->primID,
                                         ray, RTCRayN_tnear(args->ray, args->N, i), tFar, hit);
                            if(hit)
                                tFar = -std::numeric_limits<float>::infinity();
                        }
                    });
                    // TODO:intersect filter
                } break;
            }
            rtcSetGeometryMask(mGeometry.get(), gsmMask | areaLightMask);
            if(desc.transform.has_value()) {
                // TODO:use SRT
                // rtcSetGeometryTransformQuaternion
                rtcSetGeometryTransform(mGeometry.get(), 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR,
                                        desc.transform.value().A2B);  // local to world
            }
            rtcCommitGeometry(mGeometry.get());
        }
        void setCustomFunc(const void* payload, MemoryArena& arena, const GeometryIntersectFunc intersect,
                           const GeometryOccludeFunc occlude) {
            mCustomBuffer.payload = payload;
            mCustomBuffer.intersect = intersect;
            mCustomBuffer.occlude = occlude;
            auto* const data = arena.alloc<CustomBuffer>();
            *data = mCustomBuffer;
            rtcSetGeometryUserData(mGeometry.get(), data);
        }
        [[nodiscard]] Pair<CString, BuiltinTriangleBuffer> getBuiltin() const noexcept {
            return { "calcTriangleMeshSurface", mBuffer };
        }
        [[nodiscard]] RTCGeometry getGeometry() const noexcept {
            return mGeometry.get();
        }
        [[nodiscard]] RTCScene getScene(RTCDevice device) {
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
        PIPER_INTERFACE_CONSTRUCT(EmbreeNode, Node)
        virtual ~EmbreeNode() = default;
        [[nodiscard]] virtual RTCScene getScene() const noexcept = 0;
        virtual void collect(DynamicArray<GSMInstanceProgram>& gsm, DynamicArray<LightInstanceProgram>& light,
                             DynamicArray<SensorInstanceProgram>& sensor, const EmbreeTraversalNode* traversal,
                             MemoryArena& arena) = 0;
    };

    static void setTransformSRT(const RTCGeometry geometry, const uint32_t step, const TransformSRT& trans) {
        static_assert(sizeof(RTCQuaternionDecomposition) == sizeof(trans));
        static_assert(alignof(RTCQuaternionDecomposition) == alignof(TransformSRT));
        rtcSetGeometryTransformQuaternion(geometry, step, reinterpret_cast<const RTCQuaternionDecomposition*>(&trans));
    }

    // TODO:use rtcJoinCommitScene?

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
                // TODO:motion blur
                // rtcSetGeometryTimeStepCount(mGeometry.get(),1);
                if(!trans.empty()) {
                    setTransformSRT(sub.geometry.get(), 0, trans.front().second);
                }
                rtcAttachGeometry(mScene.get(), sub.geometry.get());
                rtcCommitGeometry(sub.geometry.get());
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
        EmbreeLeafNodeWithGSM(PiperContext& context, Tracer& tracer, RTCDevice device, SharedPtr<EmbreeGSMInstance> instance)
            : EmbreeNode(context), mInstance(std::move(instance)),
              mUserData(makeSharedObject<Wrapper<GSMInstanceUserData>>(context)) {
            auto& accel = dynamic_cast<EmbreeAcceleration&>(mInstance->geometry->getAcceleration(tracer));

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
        EmbreeLeafNodeWithLight(PiperContext& context, Tracer& tracer, RTCDevice device, SharedPtr<Light> light)
            : EmbreeNode(context), mLight(std::move(light)) {
            const auto* geometry = mLight->getGeometry();
            mGeometry.reset(rtcNewGeometry(device, geometry ? RTC_GEOMETRY_TYPE_INSTANCE : RTC_GEOMETRY_TYPE_USER));
            if(geometry) {
                mUserData = makeSharedObject<Wrapper<AreaLightUserData>>(context);
                auto& accel = dynamic_cast<EmbreeAcceleration&>(geometry->getAcceleration(tracer));
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
        EmbreeLeafNodeWithSensor(PiperContext& context, RTCDevice device, SharedPtr<Sensor> sensor)
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

        void collect(DynamicArray<GSMInstanceProgram>&, DynamicArray<LightInstanceProgram>&,
                     DynamicArray<SensorInstanceProgram>& sensor, const EmbreeTraversalNode* node, MemoryArena& arena) override {
            sensor.push_back(SensorInstanceProgram{ mSensor, reinterpret_cast<TraversalHandle>(node), this });
        }

        // TODO:follow
        // void postMaterialize(const TraversalHandle* follow, ResourceHolder& holder) const {}
    };

    struct EmbreeRTProgram final : public RTProgram {
        LinkableProgram program;
        String symbol;
        EmbreeRTProgram(PiperContext& context, LinkableProgram prog, String sym)
            : RTProgram(context), program(std::move(prog)), symbol(std::move(sym)) {}
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
        EmbreeProfiler mProfiler;
        uint32_t mSamplesPerPixel;

        void* upload(const SBTPayload& payload) {
            if(payload.empty())
                return nullptr;
            auto* ptr = reinterpret_cast<void*>(mArena.allocRaw(payload.size()));
            memcpy(ptr, payload.data(), payload.size());
            return ptr;
        }

    public:
        EmbreePipeline(PiperContext& context, Tracer& tracer, const PITU& pitu, SharedPtr<EmbreeNode> scene,
                       const SharedPtr<EmbreeNode>& sensor, Integrator& integrator, RenderDriver& renderDriver,
                       LightSampler& lightSampler, Sampler& sampler, uint32_t width, uint32_t height, float& ratio, bool debug)
            : Pipeline(context), mAccelerator(tracer.getAccelerator()), mArena(context.getAllocator(), 4096), mHolder(context),
              mScene(std::move(scene)), mProfiler(context) {
            DynamicArray<LinkableProgram> modules(context.getAllocator());
            modules.push_back(prepareKernelNative(context, debug));
            modules.push_back(pitu.generateLinkable(tracer.getAccelerator().getSupportedLinkableFormat()));

            DynamicArray<Pair<String, void*>> call(context.getAllocator());

            const MaterializeContext materialize{
                tracer,
                mHolder,
                mArena,
                CallSiteRegister{ [&](const SharedPtr<RTProgram>& program, const SBTPayload& payload) -> CallHandle {
                    const auto id = static_cast<uint32_t>(call.size());
                    const auto* prog = dynamic_cast<EmbreeRTProgram*>(program.get());
                    modules.emplace_back(prog->program);
                    call.emplace_back(prog->symbol, upload(payload));
                    return reinterpret_cast<CallHandle>(static_cast<ptrdiff_t>(id));
                } },
                mProfiler,
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
                auto& accel = dynamic_cast<EmbreeAcceleration&>(geometry->getAcceleration(tracer));
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

            const auto* sensorIter = std::find_if(sensors.cbegin(), sensors.cend(),
                                                  [key = eastl::dynamic_shared_pointer_cast<EmbreeLeafNodeWithSensor>(sensor)](
                                                      const SensorInstanceProgram& inst) { return key.get() == inst.node; });
            if(sensorIter == sensors.cend())
                context.getErrorHandler().raiseException("Unresolved reference node of the active sensor.",
                                                         PIPER_SOURCE_LOCATION());
            // TODO:better interface
            ratio = sensorIter->sensor->aspectRatio();
            auto RGP = sensorIter->sensor->materialize(sensorIter->traversal, materialize);
            auto& RGRTP = dynamic_cast<EmbreeRTProgram&>(*RGP.rayGen);
            modules.push_back(RGRTP.program);
            mArg.RGPayload = upload(RGP.payload);

            auto TRP = integrator.materialize(materialize);
            auto& TRRTP = dynamic_cast<EmbreeRTProgram&>(*TRP.trace);
            modules.push_back(TRRTP.program);
            mArg.TRPayload = upload(TRP.payload);

            // TODO:pass image size or real rendering window size?
            auto SAP = sampler.materialize(materialize, width, height);
            auto& start = dynamic_cast<EmbreeRTProgram&>(*SAP.start);
            auto& generate = dynamic_cast<EmbreeRTProgram&>(*SAP.generate);
            modules.push_back(start.program);
            modules.push_back(generate.program);

            mArg.SAPayload = upload(SAP.payload);
            mArg.maxDimension = SAP.maxDimension;
            mSamplesPerPixel = SAP.samplesPerPixel;

            auto& accelerator = tracer.getAccelerator();
            auto kernel = accelerator.compileKernel(Span<LinkableProgram>{ modules.data(), modules.data() + modules.size() },
                                                    String{ "piperMain", context.getAllocator() });
            mKernel = kernel.getSync();

            mArg.callInfo = mArena.alloc<CallInfo>(call.size());
            for(size_t idx = 0; idx < call.size(); ++idx) {
                mArg.callInfo[idx].address = reinterpret_cast<ptrdiff_t>(mKernel->lookup(call[idx].first));
                mArg.callInfo[idx].SBTData = call[idx].second;
            }

            for(auto& [_, prog] : geometryProg) {
                prog.calcSurface = static_cast<GeometryPostProcessFunc>(mKernel->lookup(prog.calcSurfaceFunc));
                if(prog.kind != HitKind::Builtin) {
                    prog.acceleration->setCustomFunc(prog.payload, mArena,
                                                     static_cast<GeometryIntersectFunc>(mKernel->lookup(prog.intersectFunc)),
                                                     static_cast<GeometryOccludeFunc>(mKernel->lookup(prog.occludeFunc)));
                }
            }
            for(auto& [_, prog] : surfaceProg) {
                prog.init = static_cast<SurfaceInitFunc>(mKernel->lookup(prog.initFunc));
                prog.sample = static_cast<SurfaceSampleFunc>(mKernel->lookup(prog.sampleFunc));
                prog.evaluate = static_cast<SurfaceEvaluateFunc>(mKernel->lookup(prog.evaluateFunc));
                prog.pdf = static_cast<SurfacePdfFunc>(mKernel->lookup(prog.pdfFunc));
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

            mArg.accumulate = static_cast<RenderDriverFunc>(mKernel->lookup(ACRTP.symbol));

            for(size_t idx = 0; idx < lights.size(); ++idx) {
                auto& LIP = LIPs[idx];
                auto& init = dynamic_cast<EmbreeRTProgram&>(*LIP.init);
                auto& sample = dynamic_cast<EmbreeRTProgram&>(*LIP.sample);
                auto& evaluate = dynamic_cast<EmbreeRTProgram&>(*LIP.evaluate);
                auto& pdf = dynamic_cast<EmbreeRTProgram&>(*LIP.pdf);
                mArg.lights[idx].init = static_cast<LightInitFunc>(mKernel->lookup(init.symbol));
                mArg.lights[idx].sample = static_cast<LightSampleFunc>(mKernel->lookup(sample.symbol));
                mArg.lights[idx].evaluate = static_cast<LightEvaluateFunc>(mKernel->lookup(evaluate.symbol));
                mArg.lights[idx].pdf = static_cast<LightPdfFunc>(mKernel->lookup(pdf.symbol));

                if(auto* data = lights[idx].node->postMaterialize(mHolder)) {
                    auto& geo = geometryProg[lights[idx].light->getGeometry()];
                    data->kind = geo.kind;
                    data->calcSurface = geo.calcSurface;
                    data->GEPayload = geo.payload;
                    data->usage = GeometryUsage::AreaLight;
                    data->light = reinterpret_cast<LightHandle>(mArg.lights + idx);
                }
            }

            mArg.lightSample = static_cast<LightSelectFunc>(mKernel->lookup(LSRTP.symbol));
            mArg.rayGen = static_cast<SensorFunc>(mKernel->lookup(RGRTP.symbol));
            mArg.trace = static_cast<IntegratorFunc>(mKernel->lookup(TRRTP.symbol));

            mArg.start = static_cast<SampleStartFunc>(mKernel->lookup(start.symbol));
            mArg.generate = static_cast<SampleGenerateFunc>(mKernel->lookup(generate.symbol));

            static char p1, p2, p3, p4;
            mArg.profileIntersectHit = mProfiler.registerDesc("Tracer", "Intersect Hit", &p1, StatisticsType::Bool);
            mArg.profileIntersectTime = mProfiler.registerDesc("Tracer", "Intersect Time", &p2, StatisticsType::Time);
            mArg.profileOccludeHit = mProfiler.registerDesc("Tracer", "Occlude Hit", &p3, StatisticsType::Bool);
            mArg.profileOccludeTime = mProfiler.registerDesc("Tracer", "Occlude Time", &p4, StatisticsType::Time);
            mArg.profiler = &mProfiler;
            mArg.errorHandler = &context.getErrorHandler();
        }
        void run(const RenderRECT& rect, const SBTPayload& launchData, const SensorNDCAffineTransform& transform,
                 const uint32_t sample) {
            auto stage = context().getErrorHandler().enterStage("prepare payload", PIPER_SOURCE_LOCATION());
            MemoryArena arena(context().getAllocator(), 4096);
            auto buffer = mAccelerator.createBuffer(sizeof(KernelArgument), 128);
            mArg.rect = *reinterpret_cast<const RenderRECTAlias*>(&rect);
            mArg.launchData = upload(launchData);
            mArg.sample = sample;
            mArg.transform = *reinterpret_cast<const SensorNDCAffineTransformAlias*>(&transform);
            buffer->upload(context().getScheduler().value(DataHolder{ SharedPtr<int>{}, &mArg }));

            const auto payload = mAccelerator.createPayload(InputResource{ { buffer->ref() } });

            stage.next("launch kernel", PIPER_SOURCE_LOCATION());
            const auto future =
                mAccelerator.runKernel(rect.width * rect.height, context().getScheduler().value(mKernel), payload);
            future.wait();
        }
        String generateStatisticsReport() const override {
            return mProfiler.generateReport();
        }
        uint32_t getSamplesPerPixel() const noexcept override {
            return mSamplesPerPixel;
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
        Future<SharedPtr<PITU>> mKernel;
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
            return mSampler->generateTexture(image, mode);
        }

    public:
        ResourceCacheManager& getCacheManager() override {
            return mCache;
        }
        void reportError(const RTCError ec, CString str) const {
            context().getErrorHandler().raiseException(
                "Embree Error:" + toString(context().getAllocator(), static_cast<uint32_t>(ec)) + str, PIPER_SOURCE_LOCATION());
        }
        Embree(PiperContext& context, const String& kernel, const SharedPtr<Config>& config)
            : Tracer(context), mCache(context), mKernel(context.getPITUManager().loadPITU(kernel)),
              mDebugMode(config->at("DebugMode")->get<bool>()) {
            mAccelerator = context.getModuleLoader().newInstanceT<Accelerator>(config->at("Accelerator")).getSync();
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
            if(!(rtcGetDeviceProperty(mDevice.get(), RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED) &&
                 rtcGetDeviceProperty(mDevice.get(), RTC_DEVICE_PROPERTY_TRIANGLE_GEOMETRY_SUPPORTED) &&
                 rtcGetDeviceProperty(mDevice.get(), RTC_DEVICE_PROPERTY_USER_GEOMETRY_SUPPORTED))) {
                context.getErrorHandler().raiseException(
                    "Please compile Embree with EMBREE_RAY_MASK, EMBREE_GEOMETRY_TRIANGLE, EMBREE_GEOMETRY_USER.",
                    PIPER_SOURCE_LOCATION());
            }

            // rtcSetDeviceMemoryMonitorFunction();
            mSampler = context.getModuleLoader().newInstanceT<TextureSampler>(config->at("TextureSampler")).getSync();
        }
        SharedPtr<RTProgram> buildProgram(LinkableProgram linkable, String symbol) override {
            return makeSharedObject<EmbreeRTProgram>(context(), linkable, symbol);
        }
        SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) override {
            return makeSharedObject<EmbreeAcceleration>(context(), mDevice.get(), desc);
        }
        SharedPtr<Node> buildNode(const SharedPtr<Object>& object) override {
            if(auto gsm = eastl::dynamic_shared_pointer_cast<EmbreeGSMInstance>(object))
                return makeSharedObject<EmbreeLeafNodeWithGSM>(context(), *this, mDevice.get(), std::move(gsm));
            if(auto light = eastl::dynamic_shared_pointer_cast<Light>(object)) {
                return makeSharedObject<EmbreeLeafNodeWithLight>(context(), *this, mDevice.get(), std::move(light));
            }
            if(auto sensor = eastl::dynamic_shared_pointer_cast<Sensor>(object)) {
                return makeSharedObject<EmbreeLeafNodeWithSensor>(context(), mDevice.get(), std::move(sensor));
            }
            context().getErrorHandler().raiseException(
                String{ "Unrecognized object ", context().getAllocator() } + typeid(*object).name(), PIPER_SOURCE_LOCATION());
        }
        SharedPtr<Node> buildNode(const DynamicArray<Pair<TransformInfo, SharedPtr<Node>>>& children) override {
            return makeSharedObject<EmbreeBranchNode>(context(), mDevice.get(), children);
        }
        SharedPtr<GSMInstance> buildGSMInstance(SharedPtr<Geometry> geometry, SharedPtr<Surface> surface,
                                                SharedPtr<Medium> medium) override {
            return makeSharedObject<EmbreeGSMInstance>(context(), std::move(geometry), std::move(surface), std::move(medium));
        }
        UniqueObject<Pipeline> buildPipeline(const SharedPtr<Node>& scene, const SharedPtr<Node>& sensor, Integrator& integrator,
                                             RenderDriver& renderDriver, LightSampler& lightSampler, Sampler& sampler,
                                             uint32_t width, uint32_t height, float& ratio) override {
            return makeUniqueObject<Pipeline, EmbreePipeline>(
                context(), *this, *mKernel.getSync(), eastl::dynamic_shared_pointer_cast<EmbreeNode>(scene),
                eastl::dynamic_shared_pointer_cast<EmbreeNode>(sensor), integrator, renderDriver, lightSampler, sampler, width,
                height, ratio, mDebugMode);
        }
        Accelerator& getAccelerator() override {
            return *mAccelerator;
        }
        void trace(Pipeline& pipeline, const RenderRECT& rect, const SBTPayload& launchData,
                   const SensorNDCAffineTransform& transform, const uint32_t sample) override {
            dynamic_cast<EmbreePipeline&>(pipeline).run(rect, launchData, transform, sample);
        }
        SharedPtr<Texture> generateTexture(const SharedPtr<Config>& textureDesc, const uint32_t channel) const override {
            auto res = generateTextureImpl(textureDesc);
            if(res->channel() != channel)
                context().getErrorHandler().raiseException("Mismatched channel. Expect " +
                                                               toString(context().getAllocator(), channel) + ", but get " +
                                                               toString(context().getAllocator(), res->channel()),
                                                           PIPER_SOURCE_LOCATION());
            return res;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mKernel;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, const char* path)
            : Module(context), mKernel{ String{ path, context.getAllocator() } + "/Kernel.bc" } {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Tracer") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Embree>(context(), mKernel, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
