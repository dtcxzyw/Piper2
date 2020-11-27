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
#include "../../../Interface/BuiltinComponent/Sampler.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

// https://web.maths.unsw.edu.au/~fkuo/sobol/
namespace Piper {
    class ScrambledSobolSampler final : public Sampler {
    private:
        String mKernelPath;

    public:
        ScrambledSobolSampler(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Sampler(context), mKernelPath(path + "/Kernel.bc") {
            context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }
        SamplerProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            SamplerProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            // TODO:concurrency
            pitu.wait();
            res.sample =
                tracer.buildProgram(pitu->generateLinkable(tracer.getAccelerator().getSupportedLinkableFormat()), "generate");
            // res.payload = ;
            // res.maxDimension = ;
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
            if(classID == "Sampler") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ScrambledSobolSampler>(context(), mPath, config)));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)