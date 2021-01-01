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
    class PerspectiveCamera final : public Sensor {
    private:
        String mKernelPath;
        PCData mData;
        float mAspectRatio;

    public:
        PerspectiveCamera(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Sensor(context), mKernelPath(path + "/Kernel.bc") {
            // TODO:aperture mask texture
            const auto base = parsePoint<Distance, FOR::World>(config->at("Position"));
            const auto lookAt = parsePoint<Distance, FOR::World>(config->at("LookAt"));
            const auto size = parseVector2<float>(config->at("SensorSize")) * 1e-3f;
            const auto upRef = parseVector<Distance, FOR::World>(config->at("Up"));
            // TODO:FOV
            // TODO:mm unit
            const auto focalLength = Distance{ static_cast<float>(config->at("FocalLength")->get<double>()) * 1e-3f };
            const auto apertureRadius =
                focalLength / Dimensionless<float>{ 2.0f * static_cast<float>(config->at("FStop")->get<double>()) };
            const auto forward = Normal<float, FOR::World>{ lookAt - base };
            const auto right = cross(forward, Normal<float, FOR::World>(upRef));
            const auto up = cross(right, forward);
            mData.anchor = base + up * Distance{ size.x * 0.5f } - right * Distance{ size.y * 0.5f };
            // TODO:AF/MF mode support
            mData.focalDistance = dot(lookAt - base, forward);
            const auto filmDistance = inverse(inverse(focalLength) - inverse(mData.focalDistance));
            mData.lensCenter = base + forward * filmDistance;
            mData.offX = right * Distance{ size.x };
            mData.offY = up * Distance{ -size.y };
            mData.apertureX = right * apertureRadius;
            mData.apertureY = up * apertureRadius;
            mData.forward = forward;
            mAspectRatio = size.x / size.y;
        }
        [[nodiscard]] float getAspectRatio() const noexcept override {
            return mAspectRatio;
        }
        SensorProgram materialize(Tracer& tracer, ResourceHolder& holder, const CallSiteRegister& registerCall) const override {
            SensorProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.rayGen = tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                "rayGen");
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
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
