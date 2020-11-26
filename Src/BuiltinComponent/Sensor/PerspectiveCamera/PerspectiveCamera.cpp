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
#include "../../../Interface/BuiltinComponent/Sensor.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    // TODO:DoF
    class PerspectiveCamera final : public Sensor {
    private:
        String mKernelPath;
        PCData mData;

    public:
        PerspectiveCamera(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Sensor(context), mKernelPath(path + "/Kernel.bc") {
            auto base = parsePoint<Distance, FOR::World>(config->at("Position"));
            auto lookat = parsePoint<Distance, FOR::World>(config->at("LookAt"));
            auto size = parseVector2<float>(config->at("SensorSize")) * 1e-3f;
            auto up = parseVector<Distance, FOR::World>(config->at("Up"));
            // TODO:FOV
            // TODO:mm unit
            auto focusLength = Distance{ static_cast<float>(config->at("FocusLength")->get<double>()) * 1e-3f };
            auto forward = Normal<float, FOR::World>{ lookat - base };
            auto right = cross(forward, Normal<float, FOR::World>(up));
            auto nup = cross(right, forward);
            mData.anchor = base + nup * Distance{ size.x * 0.5f } - right * Distance{ size.y * 0.5f };
            mData.focus = base + forward * focusLength;
            mData.offX = right * Distance{ size.x };
            mData.offY = nup * Distance{ -size.y };
        }
        SensorProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            SensorProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            // TODO:concurrency
            pitu.wait();
            res.rayGen =
                tracer.buildProgram(pitu->generateLinkable(tracer.getAccelerator().getSupportedLinkableFormat()), "rayGen");
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
            if(classID == "Sensor") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<PerspectiveCamera>(context(), mPath, config)));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
