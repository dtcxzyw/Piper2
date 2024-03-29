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
#include "../../../Interface/BuiltinComponent/RenderDriver.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    class SimpleFilter final : public Filter {
    private:
        String mKernelPath;
        String mFilterType;

        union Payload {
            GaussianFilterData gaussian;
        } mPayload;

    public:
        SimpleFilter(PiperContext& context, String kernel, const SharedPtr<Config>& config)
            : Filter(context), mKernelPath(std::move(kernel)), mFilterType(config->at("FilterType")->get<String>()) {
            if(mFilterType == "gaussian") {
                const auto alpha = static_cast<float>(config->at("Alpha")->get<double>());
                mPayload.gaussian.negAlpha = -alpha;
                mPayload.gaussian.sub = std::exp(-alpha);
            }
        }

        [[nodiscard]] FilterProgram materialize(const MaterializeContext& ctx) const override {
            FilterProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.weight = ctx.tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync(), mFilterType);
            res.payload = packSBTPayload(context().getAllocator(), mPayload);
            return res;
        }
    };
    // TODO: MeasuredFilter

    // TODO: unbiased estimator of sample variance
    class FixedSampler final : public RenderDriver {
    private:
        String mKernelPath;
        SharedPtr<Filter> mFilter;

    public:
        FixedSampler(PiperContext& context, String kernel, const SharedPtr<Config>& config)
            : RenderDriver(context), mKernelPath(std::move(kernel)),
              mFilter(context.getModuleLoader().newInstanceT<Filter>(config->at("Filter")).getSync()) {}
        [[nodiscard]] DynamicArray<Spectrum<Radiance>> renderFrame(Tracer& tracer, TraceLauncher& launcher) override {
            const auto [width, height] = launcher.getFilmResolution();
            auto buffer = tracer.getAccelerator().createTiledOutput(width * height * sizeof(RGBW), alignof(RGBW));

            const auto rect = launcher.getRenderRECT();
            constexpr uint32_t tileSize = 8;
            const auto blockX = (rect.width + tileSize - 1) / tileSize;
            const auto blockY = (rect.height + tileSize - 1) / tileSize;
            const auto blockCount = blockX * blockY;
            auto& logger = context().getLogger();

            DynamicArray<Future<void>> tiles{ context().getAllocator() };

            const Function<SBTPayload, uint32_t> launchData =
                [w = width, h = height, allocator = STLAllocator{ context().getAllocator() }](const uint32_t offset) {
                    return packSBTPayload(allocator, LaunchData{ offset, w, h });
                };

            SharedPtr<Resource> bufferRef = buffer;

            for(uint32_t bx = 0; bx < blockX; ++bx)
                for(uint32_t by = 0; by < blockY; ++by) {
                    const auto left = rect.left + bx * tileSize, top = rect.top + by * tileSize;
                    const auto tile = RenderRECT{ left, top, std::min(tileSize, rect.width - bx * tileSize),
                                                  std::min(tileSize, rect.height - by * tileSize) };
                    tiles.emplace_back(launcher.launch(tile, launchData, Span<SharedPtr<Resource>>{ &bufferRef, 1 }));
                }

            buffer->markDirty(context().getScheduler().wrap(tiles).raw());

            // TODO: move progress computation to Concurrency.hpp
            uint32_t progress = 0;
            while(!tiles.empty()) {
                tiles.erase(std::remove_if(tiles.begin(), tiles.end(), [](Future<void>& future) { return future.ready(); }),
                            tiles.end());

                const auto newProgress = blockCount - static_cast<uint32_t>(tiles.size());
                if(newProgress != progress) {
                    progress = newProgress;
                    if(logger.allow(LogLevel::Info))
                        logger.record(LogLevel::Info,
                                      "Progress: " +
                                          toString(context().getAllocator(),
                                                   static_cast<float>(progress) / static_cast<float>(blockCount) * 100.0f) +
                                          "% (" + toString(context().getAllocator(), progress) + "/" +
                                          toString(context().getAllocator(), blockCount) + ")",
                                      PIPER_SOURCE_LOCATION());
                }
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(100ms);
            }

            // TODO: use accelerator
            // TODO: concurrency
            auto bufferCPU = buffer->download().getSync();
            const auto bufferData = reinterpret_cast<const RGBW*>(bufferCPU.data());
            DynamicArray<Spectrum<Radiance>> res{ width * height, context().getAllocator() };
            for(size_t idx = 0; idx < res.size(); ++idx) {
                auto& rgbw = bufferData[idx];
                if(rgbw.weight.val != 0.0f)
                    res[idx] = rgbw.radiance / rgbw.weight;
                else
                    res[idx] = Spectrum<Radiance>{};
            }
            return res;
        }

        [[nodiscard]] RenderDriverProgram materialize(const MaterializeContext& ctx) const override {
            RenderDriverProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            // TODO: concurrency
            res.accumulate = ctx.tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync(), "accumulate");
            auto [sbt, prog] = mFilter->materialize(ctx);
            res.payload = packSBTPayload(context().getAllocator(), RDData{ { ctx.registerCall(prog, sbt) } });
            return res;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, const CString path)
            : Module(context), mPath{ String{ path, context.getAllocator() } + "/Kernel.bc" } {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Driver") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<FixedSampler>(context(), mPath, config)));
            }
            if(classID == "SimpleFilter") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<SimpleFilter>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
