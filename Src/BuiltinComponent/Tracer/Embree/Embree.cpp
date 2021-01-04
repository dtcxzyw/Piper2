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

#include <random>

#include <cassert>
#include <embree3/rtcore.h>
#include <spdlog/spdlog.h>
#include <utility>

// TODO:https://www.embree.org/api.html#performance-recommendations

namespace Piper {
    // TODO:controlled by RenderDriver for per tile analysis
    class EmbreeProfiler final : public Profiler {
    private:
        using BoolCounter = std::pair<std::atomic_uint64_t, std::atomic_uint64_t>;
        using FloatCounter = std::pair<std::atomic<double>, std::atomic_uint64_t>;
        using UIntCounter = UMap<std::thread::id, UMap<uint32_t, uint64_t>, std::hash<std::thread::id>>;
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
                        new(&uc) UIntCounter{ rhs.uc.get_allocator() };
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
            uint32_t id;
        };
        UMap<const void*, ItemInfo> mID;
        mutable std::mutex mMutex;
        using Clock = std::chrono::high_resolution_clock;

    public:
        explicit EmbreeProfiler(PiperContext& context)
            : Profiler(context), mStatistics(context.getAllocator()), mID(context.getAllocator()) {}
        uint32_t registerDesc(const StringView group, const StringView name, const void* uid,
                              const StatisticsType type) override {
            std::lock_guard<std::mutex> guard{ mMutex };
            const auto iter = mID.find(uid);
            if(iter != mID.cend())
                return iter->second.id;
            String sGroup{ group, context().getAllocator() }, sName{ name, context().getAllocator() };
            const auto res = static_cast<uint32_t>(mStatistics.size());
            mID.insert(makePair(uid, ItemInfo{ std::move(sGroup), std::move(sName), res }));
            auto& record = *static_cast<Record*>(mStatistics.push_back_uninitialized());
            record.type = type;
            switch(type) {
                case StatisticsType::Bool: {
                    record.bc.first = 0;
                    record.bc.second = 0;
                } break;
                case StatisticsType::UInt: {
                    new(&record.uc) UIntCounter{ context().getAllocator() };
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
        void addBool(const uint32_t id, const bool val) {
            auto& [first, second] = mStatistics[id].bc;
            ++(val ? first : second);
        }
        void addFloat(const uint32_t id, const float val) {
            auto& [first, second] = mStatistics[id].fc;
            double src = first;
            while(!first.compare_exchange_strong(src, src + static_cast<double>(val)))
                ;
            ++second;
        }
        void addTime(const uint32_t id, const uint64_t val) {
            auto& [first, second] = mStatistics[id].tc;
            first += val;
            ++second;
        }
        [[nodiscard]] static uint64_t getTime() {
            return Clock::now().time_since_epoch().count();
        }
        void addUInt(const uint32_t id, const uint32_t val) {
            const auto locate = [this, id]() -> UMap<uint32_t, uint64_t>& {
                // TODO:lock free or double check
                std::lock_guard<std::mutex> guard{ mMutex };
                auto& map = mStatistics[id].uc;
                const auto iter = map.find(std::this_thread::get_id());
                if(iter != map.cend())
                    return iter->second;
                return map.emplace(std::this_thread::get_id(), UMap<uint32_t, uint64_t>{ context().getAllocator() })
                    .first->second;
            };
            ++locate()[val];
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
                        Map<uint32_t, uint64_t> count{ context().getAllocator() };
                        for(auto&& part : record) {
                            for(auto [val, cnt] : part.second) {
                                sum += static_cast<double>(val) * static_cast<double>(cnt);
                                tot += cnt;
                                count[val] += cnt;
                            }
                        }
                        auto res = "mean " + toString(context().getAllocator(), sum / static_cast<double>(std::max(1ULL, tot))) +
                            " (" + toString(context().getAllocator(), tot) + " samples)\n";
                        for(auto [val, cnt] : count)
                            res += toString(context().getAllocator(), val) + ": " +
                                toString(context().getAllocator(),
                                         static_cast<double>(cnt) / static_cast<double>(std::max(1ULL, tot)) * 100.0) +
                                "% (" + toString(context().getAllocator(), cnt) + " samples)\n";
                        report.emplace_back(res);
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
                locate(info.group).emplace(info.name, info.name + ": " + report[info.id]);
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
    };  // namespace Piper

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
        CallInfo* callInfo;
        EmbreeProfiler* profiler;

        uint32_t profileIntersectHit;
        uint32_t profileIntersectTime;
        uint32_t profileOccludeHit;
        uint32_t profileOccludeTime;
        bool debug;
        ErrorHandler* errorHandler;
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

    void piperEmbreeStatisticsUInt(RestrictedContext* context, const uint32_t id, const uint32_t val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addUInt(id, val);
    }
    void piperEmbreeStatisticsBool(RestrictedContext* context, const uint32_t id, const bool val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addBool(id, val);
    }
    void piperEmbreeStatisticsFloat(RestrictedContext* context, const uint32_t id, const float val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addFloat(id, val);
    }
    void piperEmbreeStatisticsTime(RestrictedContext* context, const uint32_t id, const uint64_t val) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->profiler->addTime(id, val);
    }
    void piperEmbreeGetTime(RestrictedContext*, uint64_t& val) {
        val = EmbreeProfiler::getTime();
    }

    void piperEmbreeTrace(FullContext* context, const RayInfo& ray, const float minT, const float maxT, TraceResult& result) {
        auto* ctx = reinterpret_cast<KernelArgument*>(context);
        const auto begin = EmbreeProfiler::getTime();

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
            auto* scene = ctx->scene;
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
            ctx->profiler->addBool(ctx->profileIntersectHit, true);
        } else {
            result.kind = TraceKind::Missing;
            ctx->profiler->addBool(ctx->profileIntersectHit, false);
        }
        const auto end = EmbreeProfiler::getTime();
        ctx->profiler->addTime(ctx->profileIntersectTime, end - begin);
    }

    void piperEmbreeOcclude(FullContext* context, const RayInfo& ray, const float minT, const float maxT, bool& result) {
        const auto* ctx = reinterpret_cast<KernelArgument*>(context);
        const auto begin = EmbreeProfiler::getTime();
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
        ctx->profiler->addBool(ctx->profileOccludeHit, result);
        const auto end = EmbreeProfiler::getTime();
        ctx->profiler->addTime(ctx->profileOccludeTime, end - begin);
    }

    void piperEmbreeSurfaceInit(FullContext* context, const uint64_t instance, const float t, const Vector2<float>& texCoord,
                                const Normal<float, FOR::Shading>& Ng, const Face face, const TransportMode mode,
                                SurfaceStorage& storage, bool& noSpecular) {
        const auto* func = reinterpret_cast<const InstanceUserData*>(instance);
        func->init(decay(context), func->SFPayload, t, texCoord, Ng, face, mode, &storage, noSpecular);
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
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        ctx->lightSample(decay(context), ctx->LSPayload, select);
        select.light = reinterpret_cast<uint64_t>(ctx->lights + select.light);
    }
    void piperEmbreeLightInit(FullContext* context, const uint64_t light, const float t, LightStorage& storage) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->init(decay(context), func->LIPayload, t, &storage);
    }
    void piperEmbreeLightSample(FullContext* context, const uint64_t light, const LightStorage& storage,
                                const Point<Distance, FOR::World>& hit, LightSample& sample) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->sample(decay(context), func->LIPayload, &storage, hit, sample);
    }
    void piperEmbreeLightEvaluate(FullContext* context, const uint64_t light, const LightStorage& storage,
                                  const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                                  Spectrum<Radiance>& rad) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->evaluate(decay(context), func->LIPayload, &storage, lightSourceHit, dir, rad);
    }
    void piperEmbreeLightPdf(FullContext* context, uint64_t light, const LightStorage& storage,
                             const Point<Distance, FOR::World>& lightSourceHit, const Normal<float, FOR::World>& dir,
                             Dimensionless<float>& pdf) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        const auto* func = light ? reinterpret_cast<const LightFuncGroup*>(light) : ctx->lights;
        func->pdf(decay(context), func->LIPayload, &storage, lightSourceHit, dir, pdf);
    }

    void piperEmbreeQueryCall(RestrictedContext* context, const uint32_t id, CallInfo& info) {
        const auto* ctx = reinterpret_cast<const KernelArgument*>(context);
        info = ctx->callInfo[id];
    }

    // TODO:more option
    void piperEmbreePrintFloat(RestrictedContext*, const char* msg, const float ref) {
        printf("%s:%lf\n", msg, static_cast<double>(ref));
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

    static LinkableProgram prepareKernelNative(PiperContext& context, const bool debug) {
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
        PIPER_APPEND(StatisticsUInt);
        PIPER_APPEND(StatisticsBool);
        PIPER_APPEND(StatisticsFloat);
        PIPER_APPEND(StatisticsTime);
        PIPER_APPEND(GetTime);
        PIPER_APPEND(QueryCall);
        if(debug) {
            PIPER_APPEND(PrintFloat);
        }
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

    // TODO:per-vertex TBN
    static void calcTriangleMeshSurface(RestrictedContext*, const void* payload, const HitInfo& hit, float t,
                                        SurfaceIntersectionInfo& info) {
        const auto* buffer = static_cast<const BuiltinTriangleBuffer*>(payload);
        info.N = info.Ng = (hit.builtin.face == Face::Front ? hit.builtin.Ng : -hit.builtin.Ng);

        const auto pu = buffer->index[hit.builtin.index * 3 + 1], pv = buffer->index[hit.builtin.index * 3 + 2],
                   pw = buffer->index[hit.builtin.index * 3];
        const auto u = hit.builtin.barycentric.x, v = hit.builtin.barycentric.y,
                   w = 1.0f - hit.builtin.barycentric.x - hit.builtin.barycentric.y;

        const Normal<float, FOR::Local> u1{ { { 1.0f }, { 0.0f }, { 0.0f } }, Unsafe{} };
        const Normal<float, FOR::Local> u2{ { { 0.0f }, { 1.0f }, { 0.0f } }, Unsafe{} };
        if(fabsf(dot(info.N, u1).val) < fabsf(dot(info.N, u2).val))
            info.T = cross(info.N, u1);
        else
            info.T = cross(info.N, u2);
        info.B = cross(info.N, info.T);
        if(buffer->texCoord)
            info.texCoord = buffer->texCoord[pu] * u + buffer->texCoord[pv] * v + buffer->texCoord[pw] * w;
        else
            info.texCoord = { 0.0f, 0.0f };
        info.face = hit.builtin.face;
    }
    static_assert(std::is_same_v<decltype(&calcTriangleMeshSurface), GeometryFunc>);

    class EmbreeAcceleration final : public AccelerationStructure {
    private:
        GeometryHandle mGeometry;
        SceneHandle mScene;
        GeometryFunc mBuiltin;
        BuiltinTriangleBuffer mBuffer;

    public:
        EmbreeAcceleration(PiperContext& context, RTCDevice device, const GeometryDesc& desc)
            : AccelerationStructure(context), mBuffer{} {
            switch(desc.type) {
                case PrimitiveShapeType::TriangleIndexed: {
                    mBuiltin = calcTriangleMeshSurface;

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
                    rtcSetGeometryMask(mGeometry.get(), 1);
                    if(triDesc.transform.has_value())
                        rtcSetGeometryTransform(mGeometry.get(), 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, triDesc.transform.value().A2B);

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

                    rtcCommitGeometry(mGeometry.get());
                } break;
                default:
                    context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            }
        }
        [[nodiscard]] Pair<GeometryFunc, BuiltinTriangleBuffer> getBuiltinFunc() const noexcept {
            return { mBuiltin, mBuffer };
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
        EmbreeBranchNode(PiperContext& context, RTCDevice device, const GroupDesc& instances)
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
        EmbreeLeafNode(PiperContext& context, Tracer& tracer, RTCDevice device, GSMInstanceDesc gsm)
            : EmbreeNode(context), mInstance(std::move(gsm)) {
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

        void* upload(const SBTPayload& payload) {
            if(payload.empty())
                return nullptr;
            auto* ptr = reinterpret_cast<void*>(mArena.allocRaw(payload.size()));
            memcpy(ptr, payload.data(), payload.size());
            return ptr;
        }

    public:
        EmbreePipeline(PiperContext& context, Tracer& tracer, const SharedPtr<EmbreeNode>& scene, Sensor& sensor,
                       Integrator& integrator, RenderDriver& renderDriver, const LightSampler& lightSampler,
                       const Span<SharedPtr<Light>>& lights, Sampler* sampler, uint32_t width, uint32_t height, bool debug)
            : Pipeline(context), mAccelerator(tracer.getAccelerator()), mArena(context.getAllocator(), 4096), mHolder(context),
              mScene(scene), mProfiler(context) {
            DynamicArray<LinkableProgram> modules(context.getAllocator());
            modules.push_back(prepareKernelNative(context, debug));

            DynamicArray<Pair<String, void*>> call(context.getAllocator());

            const MaterializeContext materialize{
                tracer, mHolder, CallSiteRegister{ [&](const SharedPtr<RTProgram>& program, const SBTPayload& payload) {
                    const auto id = static_cast<size_t>(call.size());
                    const auto* prog = dynamic_cast<EmbreeRTProgram*>(program.get());
                    modules.emplace_back(prog->program);
                    call.emplace_back(prog->symbol, upload(payload));
                    return id;
                } },
                mProfiler, TextureLoader{ [&](const SharedPtr<Config>& desc, const uint32_t channel) -> uint32_t {
                    const auto texture = tracer.generateTexture(desc, channel);
                    auto [SBT, prog] = texture->materialize(materialize);
                    return materialize.registerCall(prog, SBT);
                } }
            };

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
                    auto sp = prog.surface->materialize(materialize);
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
                    auto gp = prog.geometry->materialize(materialize);
                    auto& info = geometryProg[prog.geometry.get()];

                    if(gp.surface) {
                        info.kind = HitKind::Custom;
                        info.payload = upload(gp.payload);
                        auto& surface = dynamic_cast<EmbreeRTProgram&>(*gp.surface);
                        info.calcSurfaceFunc = surface.symbol;
                        modules.push_back(surface.program);
                    } else {
                        auto& accel = dynamic_cast<EmbreeAcceleration&>(prog.geometry->getAcceleration(tracer));
                        auto [func, payload] = accel.getBuiltinFunc();
                        info.calcSurface = func;
                        info.payload = upload(packSBTPayload(context.getAllocator(), payload));
                        info.kind = HitKind::Builtin;
                    }
                }
            }

            auto ACP = renderDriver.materialize(materialize);
            auto& ACRTP = dynamic_cast<EmbreeRTProgram&>(*ACP.accumulate);
            modules.push_back(ACRTP.program);
            mArg.ACPayload = nullptr;

            auto LSP = lightSampler.materialize(materialize);
            auto& LSRTP = dynamic_cast<EmbreeRTProgram&>(*LSP.select);
            modules.push_back(LSRTP.program);
            mArg.LSPayload = upload(LSP.payload);

            mArg.lights = mArena.alloc<LightFuncGroup>(lights.size());

            DynamicArray<LightProgram> LIPs{ context.getAllocator() };
            for(size_t idx = 0; idx < lights.size(); ++idx) {
                auto&& light = lights[idx];
                auto LIP = light->materialize(materialize);
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
            }

            auto RGP = sensor.materialize(materialize);
            auto& RGRTP = dynamic_cast<EmbreeRTProgram&>(*RGP.rayGen);
            modules.push_back(RGRTP.program);
            mArg.RGPayload = upload(RGP.payload);

            auto TRP = integrator.materialize(materialize);
            auto& TRRTP = dynamic_cast<EmbreeRTProgram&>(*TRP.trace);
            modules.push_back(TRRTP.program);
            mArg.TRPayload = upload(TRP.payload);

            String sampleSymbol;
            if(sampler) {
                auto SAP = sampler->materialize(materialize);
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
            mKernel = kernel.getSync();

            mArg.callInfo = mArena.alloc<CallInfo>(call.size());
            for(size_t idx = 0; idx < call.size(); ++idx) {
                mArg.callInfo[idx].address = reinterpret_cast<uint64_t>(mKernel->lookup(call[idx].first));
                mArg.callInfo[idx].SBTData = call[idx].second;
            }

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
                auto& LIP = LIPs[idx];
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

            static char p1, p2, p3, p4;
            mArg.profileIntersectHit = mProfiler.registerDesc("Tracer", "Intersect Hit", &p1, StatisticsType::Bool);
            mArg.profileIntersectTime = mProfiler.registerDesc("Tracer", "Intersect Time", &p2, StatisticsType::Time);
            mArg.profileOccludeHit = mProfiler.registerDesc("Tracer", "Occlude Hit", &p3, StatisticsType::Bool);
            mArg.profileOccludeTime = mProfiler.registerDesc("Tracer", "Occlude Time", &p4, StatisticsType::Time);
            mArg.profiler = &mProfiler;
            mArg.errorHandler = &context.getErrorHandler();
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
        String generateStatisticsReport() const override {
            return mProfiler.generateReport();
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
        Embree(PiperContext& context, const SharedPtr<Config>& config)
            : Tracer(context), mCache(context), mDebugMode(config->at("DebugMode")->get<bool>()) {
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
            // rtcSetDeviceMemoryMonitorFunction();
            mSampler = context.getModuleLoader().newInstanceT<TextureSampler>(config->at("TextureSampler")).getSync();
        }
        SharedPtr<RTProgram> buildProgram(LinkableProgram linkable, String symbol) override {
            return makeSharedObject<EmbreeRTProgram>(context(), linkable, symbol);
        }
        SharedPtr<AccelerationStructure> buildAcceleration(const GeometryDesc& desc) override {
            return makeSharedObject<EmbreeAcceleration>(context(), mDevice.get(), desc);
        }
        SharedPtr<Node> buildNode(const NodeDesc& desc) override {
            if(desc.index() == 0)
                return makeSharedObject<EmbreeBranchNode>(context(), mDevice.get(), eastl::get<GroupDesc>(desc));
            // TODO:move
            return makeSharedObject<EmbreeLeafNode>(context(), *this, mDevice.get(), eastl::get<GSMInstanceDesc>(desc));
        }
        UniqueObject<Pipeline> buildPipeline(SharedPtr<Node> scene, Sensor& sensor, Integrator& integrator,
                                             RenderDriver& renderDriver, const LightSampler& lightSampler,
                                             const Span<SharedPtr<Light>>& lights, Sampler* sampler, uint32_t width,
                                             uint32_t height) override {
            return makeUniqueObject<Pipeline, EmbreePipeline>(
                context(), *this, eastl::dynamic_shared_pointer_cast<EmbreeNode>(scene), sensor, integrator, renderDriver,
                lightSampler, lights, sampler, width, height, mDebugMode);
        }
        Accelerator& getAccelerator() override {
            return *mAccelerator;
        }
        void trace(Pipeline& pipeline, const RenderRECT& rect, const SBTPayload& renderDriverPayload,
                   const SensorNDCAffineTransform& transform, const uint32_t sample) override {
            dynamic_cast<EmbreePipeline&>(pipeline).run(rect, renderDriverPayload, transform, sample);
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
