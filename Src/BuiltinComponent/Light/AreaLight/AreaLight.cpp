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
#include "../../../Interface/BuiltinComponent/Geometry.hpp"
#include "../../../Interface/BuiltinComponent/Light.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#include "Shared.hpp"

namespace Piper {
    class Diffuse final : public Light {
    private:
        String mKernelPath;
        SharedPtr<Geometry> mGeometry;
        Area<float> mArea;
        Spectrum<Radiance> mRadiance;

    public:
        Diffuse(PiperContext& context, const SharedPtr<Config>& config, String kernel)
            : Light(context), mKernelPath(std::move(kernel)),
              mGeometry{ context.getModuleLoader().newInstanceT<Geometry>(config->at("Geometry")).getSync() },
              mArea{ mGeometry->area() }, mRadiance{ parseSpectrum<Radiance>(config->at("Radiance")) } {}

        [[nodiscard]] const Geometry* getGeometry() const override {
            return mGeometry.get();
        }
        [[nodiscard]] LightProgram materialize(TraversalHandle traversal, const MaterializeContext& ctx) const override {
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            LightProgram res;
            res.init = ctx.tracer.buildProgram(linkable, "diffuseInit");
            res.sample = ctx.tracer.buildProgram(linkable, "diffuseSample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "diffuseEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "diffusePdf");
            const auto shape = mGeometry->materialize(traversal, ctx);
            res.payload = packSBTPayload(context().getAllocator(),
                                         DiffuseAreaLightData{ mRadiance, ctx.registerCall(shape.sample, shape.payload), mArea });
            return res;
        }

        [[nodiscard]] LightAttributes attributes() const noexcept override {
            return LightAttributes::Area;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mKernelPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path) : Module(context), mKernelPath(path, context.getAllocator()) {
            mKernelPath += "/Kernel.bc";
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Diffuse") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Diffuse>(context(), config, mKernelPath)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
