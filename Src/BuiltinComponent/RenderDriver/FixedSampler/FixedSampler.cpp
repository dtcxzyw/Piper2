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
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                mFilterType);
            res.payload = packSBTPayload(context().getAllocator(), mPayload);
            return res;
        }
    };
    // TODO:MeasuredFilter

    // TODO:tiled
    class FixedSampler final : public RenderDriver {
    private:
        String mKernelPath;
        SharedPtr<Filter> mFilter;

    public:
        FixedSampler(PiperContext& context, String kernel, const SharedPtr<Config>& config)
            : RenderDriver(context), mKernelPath(std::move(kernel)),
              mFilter(context.getModuleLoader().newInstanceT<Filter>(config->at("Filter")).getSync()) {}
        void renderFrame(DynamicArray<Spectrum<Radiance>>& res, const uint32_t width, const uint32_t height,
                         const RenderRECT& rect, const SensorNDCAffineTransform& transform, Tracer& tracer,
                         TraceLauncher& launcher) override {
            // TODO:use buffer (pass dependencies to tracer)
            // auto buffer = tracer.getAccelerator().createBuffer(width * height * sizeof(Spectrum<Radiance>), 128);
            // payload.res = reinterpret_cast<Spectrum<Radiance>*>(buffer->ref()->getHandle());
            // buffer->reset();
            DynamicArray<RGBW> buffer{ res.size(), context().getAllocator() };
            const auto spp = launcher.getSamplesPerPixel();
            for(uint32_t i = 0; i < spp; ++i) {
                auto stage = context().getErrorHandler().enterStage("progress " + toString(context().getAllocator(), i + 1) +
                                                                        "/" + toString(context().getAllocator(), spp),
                                                                    PIPER_SOURCE_LOCATION());
                launcher.launch(rect, packSBTPayload(context().getAllocator(), LaunchData{ buffer.data(), width, height }),
                                transform, i);
            }

            // auto bufferCPU = buffer->download();
            // bufferCPU.wait();
            // memcpy(res.data(),bufferCPU->data(), bufferCPU->size());
            for(size_t idx = 0; idx < res.size(); ++idx) {
                auto& rgbw = buffer[idx];
                if(rgbw.weight.val != 0.0f)
                    res[idx] = rgbw.radiance / rgbw.weight;
                else
                    res[idx] = Spectrum<Radiance>{};
            }
        }

        [[nodiscard]] RenderDriverProgram materialize(const MaterializeContext& ctx) const override {
            RenderDriverProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.accumulate = ctx.tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                "accumulate");
            auto [sbt, prog] = mFilter->materialize(ctx);
            res.payload = packSBTPayload(context().getAllocator(), RDData{ ctx.registerCall(prog, sbt) });
            return res;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path)
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
