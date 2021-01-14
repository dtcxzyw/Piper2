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
#include "../../../Interface/BuiltinComponent/Integrator.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Profiler.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    // TODO:Russian Roulette
    class PathTracing final : public Integrator {
    private:
        String mKernelPath;
        uint32_t mMaxDepth;

    public:
        PathTracing(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Integrator(context), mKernelPath(path + "/Kernel.bc") {
            mMaxDepth = static_cast<uint32_t>(config->at("MaxTraceDepth")->get<uintmax_t>());
        }

        [[nodiscard]] IntegratorProgram materialize(const MaterializeContext& ctx) const override {
            IntegratorProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.trace = ctx.tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                "trace");
            static char p1, p2, p3;
            res.payload = packSBTPayload(
                context().getAllocator(),
                Data{ mMaxDepth, ctx.profiler.registerDesc("Integrator", "Trace Depth", &p1, StatisticsType::UInt, mMaxDepth + 1),
                      ctx.profiler.registerDesc("Integrator", "Time Per Path", &p2, StatisticsType::Time),
                      ctx.profiler.registerDesc("Integrator", "Valid Rays", &p3, StatisticsType::Bool) });
            return res;
        }
    };  // namespace Piper
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path) : Module(context), mPath(path, context.getAllocator()) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Integrator") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<PathTracing>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
