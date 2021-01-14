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
#include "../../../Kernel/Protocol.hpp"
#include <random>

struct RTCSceneTy;

namespace Piper {
    struct RenderRECTAlias final {
        uint32_t left, top, width, height;
    };

    struct SensorNDCAffineTransformAlias final {
        float ox, oy, sx, sy;
    };

    class EmbreeProfiler;
    class ErrorHandler;

    struct LightFuncGroup final {
        LightInitFunc init;
        LightSampleFunc sample;
        LightEvaluateFunc evaluate;
        LightPdfFunc pdf;
        const void* LIPayload;
    };

    enum class HitKind { Builtin, Custom };
    enum class GeometryUsage { GSM, AreaLight };

    struct GeometryUserData {
        HitKind kind;
        GeometryPostProcessFunc calcSurface;
        const void* GEPayload;
        GeometryUsage usage;
    };

    struct GSMInstanceUserData final : public GeometryUserData {
        SurfaceInitFunc init;
        SurfaceSampleFunc sample;
        SurfaceEvaluateFunc evaluate;
        SurfacePdfFunc pdf;

        const void* SFPayload;
        // TODO:medium
    };

    struct AreaLightUserData final : public GeometryUserData {
        LightHandle light;
    };

    struct KernelArgument final {
        uint32_t width, height;

        RenderRECTAlias rect;
        RTCSceneTy* scene;
        SensorFunc rayGen;
        const void* RGPayload;
        RenderDriverFunc accumulate;
        const void* ACPayload;
        const void* launchData;
        IntegratorFunc trace;
        const void* TRPayload;
        LightFuncGroup* lights;
        LightSelectFunc lightSample;
        const void* LSPayload;
        SampleStartFunc start;
        SampleGenerateFunc generate;
        const void* SAPayload;
        uint32_t sampleCount;
        uint32_t maxDimension;
        SensorNDCAffineTransformAlias transform;
        CallInfo* callInfo;
        EmbreeProfiler* profiler;

        StatisticsHandle profileIntersectHit;
        StatisticsHandle profileIntersectTime;
        StatisticsHandle profileOccludeHit;
        StatisticsHandle profileOccludeTime;
        StatisticsHandle profileSampleTime;
        bool debug;
        ErrorHandler* errorHandler;
    };
    using RandomEngine = std::mt19937_64;
    struct PerSampleContext final {
        KernelArgument argument;
        Time<float> time;
        uint32_t currentDimension;
        uint64_t sampleIndex;

        RandomEngine eng;
    };

    struct BuiltinHitInfo final {
        Normal<float, FOR::Local> Ng;
        Vector2<float> barycentric;
        uint32_t index;
        Face face;
    };
    static_assert(sizeof(BuiltinHitInfo) <= sizeof(GeometryStorage));
}  // namespace Piper
