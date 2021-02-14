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
#include "../../../Interface/BuiltinComponent/Sampler.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

// https://web.maths.unsw.edu.au/~fkuo/sobol/
// based on recommended new-joe-kuo-6.21201 data
// TODO: Optimization of the Direction Numbers of the Sobol Sequences.
// TODO: Enumerating Quasi-Monte Carlo Point Sequences in Elementary Intervals
// TODO: tiled-rendering
namespace Piper {
    class SobolSampler final : public Sampler {
    private:
        String mKernelPath;
        uint32_t mSamplesPerPixel;
        uint32_t mScramble;

    public:
        SobolSampler(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Sampler(context), mKernelPath(path + "/Kernel.bc"),
              mSamplesPerPixel(static_cast<uint32_t>(config->at("SamplesPerPixel")->get<uintmax_t>())),
              mScramble(static_cast<uint32_t>(config->at("Scramble")->get<uintmax_t>())) {
#pragma warning(push, 4)
#pragma warning(disable : 4146)
            if((mSamplesPerPixel & -mSamplesPerPixel) != mSamplesPerPixel) {
#pragma warning(pop)
                mSamplesPerPixel = 1U << static_cast<uint32_t>(std::ceil(std::log2(static_cast<double>(mSamplesPerPixel))));
                if(context.getLogger().allow(LogLevel::Warning))
                    context.getLogger().record(LogLevel::Warning,
                                               "The samples per pixel is not the power of 2. It was rounded up to " +
                                                   toString(context.getAllocator(), mSamplesPerPixel),
                                               PIPER_SOURCE_LOCATION());
            }
        }

        [[nodiscard]] SamplerProgram materialize(const MaterializeContext& ctx) const override {
            SamplerProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            res.start = ctx.tracer.buildProgram(linkable, "sobolStart");
            res.generate = ctx.tracer.buildProgram(linkable, "sobolGenerate");
            return res;
        }

        [[nodiscard]] SamplerAttributes generatePayload(const uint32_t width, const uint32_t height) const override {
            const auto log2Resolution = static_cast<uint32_t>(std::ceil(std::log2(static_cast<double>(std::max(width, height)))));
            return { packSBTPayload(context().getAllocator(),
                                    SobolData{ static_cast<uint32_t>(1U << log2Resolution), log2Resolution, mScramble }),
                     numSobolDimensions, mSamplesPerPixel };
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
            if(classID == "Sampler") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<SobolSampler>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
