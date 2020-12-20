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
#include "../../../Interface/BuiltinComponent/Light.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    class PointLight final : public Light {
    private:
        String mKernelPath;
        PointLightData mData;

    public:
        PointLight(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Light(context), mKernelPath(path + "/Kernel.bc") {
            // TODO:use UnitManager
            // TODO:transform
            mData.pos = parsePoint<Distance, FOR::World>(config->at("Position"));
            mData.intensity = parseSpectrum<Intensity>(config->at("Intensity"));
        }
        LightProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            LightProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.light = tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                "lightPoint");
            res.payload = packSBTPayload(context().getAllocator(), mData);
            return res;
        }
        bool isDelta() const override {
            return true;
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
            if(classID == "Point") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<PointLight>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
