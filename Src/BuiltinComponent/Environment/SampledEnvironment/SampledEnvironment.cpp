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
#include "../../../Interface/BuiltinComponent/Environment.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    // TODO:use texture
    class ConstantBackground final : public Environment {
    private:
        String mKernelPath;
        Data mData;

    public:
        ConstantBackground(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Environment(context), mKernelPath(path + "/Kernel.bc") {
            mData.background = parseSpectrum<Radiance>(config->at("Radiance"));
        }
        EnvironmentProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            EnvironmentProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.missing = tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat()), "missing");
            res.payload = packSBTPayload(context().getAllocator(), mData);
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
            if(classID == "Constant") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ConstantBackground>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
