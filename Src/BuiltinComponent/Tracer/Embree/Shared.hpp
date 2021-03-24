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
        Call<LightInitFunc> init;
        Call<LightSampleFunc> sample;
        Call<LightEvaluateFunc> evaluate;
        Call<LightPdfFunc> pdf;
    };

    enum class HitKind { Builtin, Custom };
    enum class GeometryUsage { GSM, AreaLight };

    struct GeometryUserData {
        HitKind kind;
        Call<GeometryPostProcessFunc> calcSurface;
        GeometryUsage usage;
    };

    struct GSMInstanceUserData final : GeometryUserData {
        Call<SurfaceInitFunc> init;
        Call<SurfaceSampleFunc> sample;
        Call<SurfaceEvaluateFunc> evaluate;
        Call<SurfacePdfFunc> pdf;

        // TODO:medium
    };

    struct AreaLightUserData final : GeometryUserData {
        LightHandle light;
    };

    struct KernelArgument final {
        uint32_t width, height;

        RenderRECTAlias rect;
        RenderRECTAlias fullRect;
        RTCSceneTy* scene;
        Call<SensorFunc> rayGen;
        const void* ACPayload;
        const void* launchData;
        const void* TRPayload;
        LightFuncGroup* lights;
        LightSelectFunc lightSample;
        const void* LSPayload;
        const void* SAPayload;
        uint32_t sampleCount;
        uint32_t maxDimension;
        SensorNDCAffineTransformAlias transform;
        const void* const* callInfo;
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
        TaskContext ctx;
        ResourceHandle root;
        void** symbolLUT;
        Time<float> time;
        uint32_t currentDimension;
        uint64_t sampleIndex;

        RandomEngine randomEngine;
    };

    struct BuiltinHitInfo final {
        Normal<float, FOR::Local> Ng;
        Vector2<float> barycentric;
        uint32_t index;
        Face face;
    };
    static_assert(sizeof(BuiltinHitInfo) <= sizeof(GeometryStorage));
}  // namespace Piper
