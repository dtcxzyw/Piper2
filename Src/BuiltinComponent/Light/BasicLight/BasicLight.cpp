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
#include <utility>

namespace Piper {
    class SampledEnvironment final : public Light {
    private:
        String mKernelPath;
        SampledEnvironmentData mData;

    public:
        SampledEnvironment(PiperContext& context, String path, const SharedPtr<Config>& config)
            : Light(context), mKernelPath(std::move(path)) {
            mData.texture = parseSpectrum<Radiance>(config->at("Radiance"));
        }

        [[nodiscard]] LightProgram materialize(const MaterializeContext& ctx) const override {
            LightProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto program =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(program, "SEInit");
            res.sample = ctx.tracer.buildProgram(program, "SESample");
            res.evaluate = ctx.tracer.buildProgram(program, "SEEvaluate");
            res.pdf = ctx.tracer.buildProgram(program, "SEPdf");
            res.payload = packSBTPayload(context().getAllocator(), mData);
            return res;
        }

        [[nodiscard]] bool isDelta() const noexcept override {
            return false;
        }
    };

    class PointLight final : public Light {
    private:
        String mKernelPath;
        PointLightData mData;

    public:
        PointLight(PiperContext& context, String path, const SharedPtr<Config>& config)
            : Light(context), mKernelPath(std::move(path)) {
            // TODO:use UnitManager
            // TODO:transform
            mData.pos = parsePoint<Distance, FOR::World>(config->at("Position"));
            mData.intensity = parseSpectrum<Intensity>(config->at("Intensity"));
        }

        [[nodiscard]] LightProgram materialize(const MaterializeContext& ctx) const override {
            LightProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto program =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(program, "pointInit");
            res.sample = ctx.tracer.buildProgram(program, "pointSample");
            res.evaluate = ctx.tracer.buildProgram(program, "deltaEvaluate");
            res.pdf = ctx.tracer.buildProgram(program, "deltaPdf");
            res.payload = packSBTPayload(context().getAllocator(), mData);
            return res;
        }

        [[nodiscard]] bool isDelta() const noexcept override {
            return true;
        }
    };
    class UniformLightSampler final : public LightSampler {
    private:
        String mKernelPath;
        DynamicArray<bool> mFlags;

    public:
        UniformLightSampler(PiperContext& context, String path)
            : LightSampler(context), mKernelPath(std::move(path)), mFlags(context.getAllocator()) {}
        void preprocess(const Span<const SharedPtr<Light>>& lights) override {
            if(lights.empty())
                context().getErrorHandler().assertFailed(ErrorHandler::CheckLevel::InterfaceArgument, "Need environment",
                                                         PIPER_SOURCE_LOCATION());
            for(auto&& light : lights)
                mFlags.push_back(light->isDelta());
        }

        [[nodiscard]] LightSamplerProgram materialize(const MaterializeContext& ctx) const override {
            LightSamplerProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto program =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            res.select = ctx.tracer.buildProgram(program, "uniformSelect");
            DynamicArray<std::byte> data(context().getAllocator());
            const auto size = static_cast<uint32_t>(mFlags.size());
            data.insert(data.cend(), reinterpret_cast<const std::byte*>(&size),
                        reinterpret_cast<const std::byte*>(&size) + sizeof(uint32_t));
            data.insert(data.cend(), reinterpret_cast<const std::byte*>(mFlags.data()),
                        reinterpret_cast<const std::byte*>(mFlags.data()) + mFlags.size());
            res.payload = std::move(data);
            return res;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path) : Module(context), mPath(path, context.getAllocator()) {
            mPath += "/Kernel.bc";
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Point") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<PointLight>(context(), mPath, config)));
            }
            if(classID == "SampledEnvironment") {
                return context().getScheduler().constructObject<SampledEnvironment>(mPath, config);
            }
            if(classID == "UniformLightSampler") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<UniformLightSampler>(context(), mPath)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
