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
#include <utility>
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/BuiltinComponent/Surface.hpp"
#include "../../../Interface/BuiltinComponent/Texture.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    class BlackBody final : public Surface {
    private:
        String mKernelPath;

    public:
        BlackBody(PiperContext& context, String path) : Surface(context), mKernelPath(std::move(path)) {}
        SurfaceProgram materialize(const MaterializeContext& ctx) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(linkable, "blackBodyInit");
            res.sample = ctx.tracer.buildProgram(linkable, "blackBodySample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "blackBodyEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "blackBodyPdf");
            return res;
        }
    };

    /*
    class DisneyPrincipledBRDF final : public Surface {
    private:
        String mKernelPath;
        DisneyPrincipledBRDFData mData;

    public:
        DisneyPrincipledBRDF(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Surface(context), mKernelPath(path + "/Kernel.bc") {}
        SurfaceProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable = PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat());
            res.sample = tracer.buildProgram(linkable, "sample");
            res.evaluate = tracer.buildProgram(linkable, "evaluate");
            res.payload = packSBTPayload(context().getAllocator(), mData);
            return res;
        }
    };
    */

    class Matte final : public Surface {
    private:
        String mKernelPath;
        SharedPtr<Config> mDiffuse, mRoughness;

    public:
        Matte(PiperContext& context, const SharedPtr<Config>& config, String path)
            : Surface(context), mKernelPath(std::move(path)), mDiffuse(config->at("Diffuse")),
              mRoughness(config->at("Roughness")) {}
        SurfaceProgram materialize(const MaterializeContext& ctx) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(linkable, "matteInit");
            res.sample = ctx.tracer.buildProgram(linkable, "matteSample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "matteEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "mattePdf");

            const MatteData data{ ctx.loadTexture(mDiffuse, 4), ctx.loadTexture(mRoughness, 1) };
            res.payload = packSBTPayload(context().getAllocator(), data);
            return res;
        }
    };

    class Glass final : public Surface {
    private:
        String mKernelPath;
        SharedPtr<Config> mReflection, mTransmission, mRoughnessX, mRoughnessY;

    public:
        Glass(PiperContext& context, const SharedPtr<Config>& config, String path)
            : Surface(context), mKernelPath(std::move(path)), mReflection(config->at("Reflection")),
              mTransmission(config->at("Transmission")), mRoughnessX(config->at("RoughnessX")),
              mRoughnessY(config->at("RoughnessY")) {}

        [[nodiscard]] SurfaceProgram materialize(const MaterializeContext& ctx) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(linkable, "glassInit");
            res.sample = ctx.tracer.buildProgram(linkable, "glassSample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "glassEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "glassPdf");

            const GlassData data{ ctx.loadTexture(mReflection, 4), ctx.loadTexture(mTransmission, 4),
                                  ctx.loadTexture(mRoughnessX, 1), ctx.loadTexture(mRoughnessY, 1) };
            res.payload = packSBTPayload(context().getAllocator(), data);
            return res;
        }
    };

    class Plastic final : public Surface {
    private:
        String mKernelPath;
        SharedPtr<Config> mDiffuse, mSpecular, mRoughness;

    public:
        Plastic(PiperContext& context, const SharedPtr<Config>& config, String path)
            : Surface(context), mKernelPath(std::move(path)), mDiffuse(config->at("Diffuse")), mSpecular(config->at("Specular")),
              mRoughness(config->at("Roughness")) {}

        [[nodiscard]] SurfaceProgram materialize(const MaterializeContext& ctx) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(linkable, "plasticInit");
            res.sample = ctx.tracer.buildProgram(linkable, "plasticSample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "plasticEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "plasticPdf");

            const PlasticData data{ ctx.loadTexture(mDiffuse, 4), ctx.loadTexture(mSpecular, 4), ctx.loadTexture(mRoughness, 1) };
            res.payload = packSBTPayload(context().getAllocator(), data);
            return res;
        }
    };

    class Mirror final : public Surface {
    private:
        String mKernelPath;
        SharedPtr<Config> mReflection;

    public:
        Mirror(PiperContext& context, const SharedPtr<Config>& config, String path)
            : Surface(context), mKernelPath(std::move(path)), mReflection(config->at("Reflection")) {}

        [[nodiscard]] SurfaceProgram materialize(const MaterializeContext& ctx) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(linkable, "mirrorInit");
            res.sample = ctx.tracer.buildProgram(linkable, "mirrorSample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "blackBodyEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "blackBodyPdf");

            const MirrorData data{ ctx.loadTexture(mReflection, 4) };
            res.payload = packSBTPayload(context().getAllocator(), data);
            return res;
        }
    };

    class Substrate final : public Surface {
    private:
        String mKernelPath;
        SharedPtr<Config> mDiffuse, mSpecular, mRoughnessX, mRoughnessY;

    public:
        Substrate(PiperContext& context, const SharedPtr<Config>& config, String path)
            : Surface(context), mKernelPath(std::move(path)), mDiffuse(config->at("Diffuse")), mSpecular(config->at("Specular")),
              mRoughnessX(config->at("RoughnessX")), mRoughnessY(config->at("RoughnessY")) {}

        [[nodiscard]] SurfaceProgram materialize(const MaterializeContext& ctx) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();
            res.init = ctx.tracer.buildProgram(linkable, "substrateInit");
            res.sample = ctx.tracer.buildProgram(linkable, "substrateSample");
            res.evaluate = ctx.tracer.buildProgram(linkable, "substrateEvaluate");
            res.pdf = ctx.tracer.buildProgram(linkable, "substratePdf");

            const SubstrateData data{ ctx.loadTexture(mDiffuse, 4), ctx.loadTexture(mSpecular, 4),
                                      ctx.loadTexture(mRoughnessX, 1), ctx.loadTexture(mRoughnessY, 1) };
            res.payload = packSBTPayload(context().getAllocator(), data);
            return res;
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
            if(classID == "BlackBody") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<BlackBody>(context(), mKernelPath)));
            }
            if(classID == "Matte") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Matte>(context(), config, mKernelPath)));
            }
            if(classID == "Plastic") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Plastic>(context(), config, mKernelPath)));
            }
            if(classID == "Mirror") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Mirror>(context(), config, mKernelPath)));
            }
            if(classID == "Glass") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Glass>(context(), config, mKernelPath)));
            }
            if(classID == "Substrate") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Substrate>(context(), config, mKernelPath)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
