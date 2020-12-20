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
    // TODO:tiled
    class FixedSampler final : public RenderDriver {
    private:
        String mKernelPath;
        uint32_t mSample;

    public:
        FixedSampler(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : RenderDriver(context), mKernelPath(path + "/Kernel.bc") {
            mSample = static_cast<uint32_t>(config->at("SPP")->get<uintmax_t>());
        }
        void renderFrame(DynamicArray<Spectrum<Radiance>>& res, const uint32_t width, const uint32_t height,
                         const RenderRECT& rect, const SensorNDCAffineTransform& transform, Tracer& tracer,
                         Pipeline& pipeline) override {
            // TODO:use buffer (pass dependencies to tracer)
            // auto buffer = tracer.getAccelerator().createBuffer(width * height * sizeof(Spectrum<Radiance>), 128);
            Data payload;
            payload.w = width;
            payload.h = height;
            // payload.res = reinterpret_cast<Spectrum<Radiance>*>(buffer->ref()->getHandle());
            // buffer->reset();
            payload.res = res.data();

            for(uint32_t i = 0; i < mSample; ++i) {
                auto stage = context().getErrorHandler().enterStage("progress " + toString(context().getAllocator(), i + 1) +
                                                                        "/" + toString(context().getAllocator(), mSample),
                                                                    PIPER_SOURCE_LOCATION());
                tracer.trace(pipeline, rect, packSBTPayload(context().getAllocator(), payload), transform, i);
            }

            // auto bufferCPU = buffer->download();
            // bufferCPU.wait();
            // memcpy(res.data(),bufferCPU->data(), bufferCPU->size());
            for(auto&& pixel : res) {
                pixel = pixel / Dimensionless<float>{ static_cast<float>(mSample) };
                // printf("%f %f %f\n", pixel.r.val, pixel.g.val, pixel.b.val);
            }
        }
        RenderDriverProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            RenderDriverProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.accumulate = tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                "accumulate");
            return res;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path) : Module(context), mPath(path, context.getAllocator()) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Driver") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<FixedSampler>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
