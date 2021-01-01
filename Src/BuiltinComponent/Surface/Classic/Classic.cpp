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
        SurfaceProgram materialize(Tracer& tracer, ResourceHolder& holder, const CallSiteRegister& registerCall) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            res.init = tracer.buildProgram(linkable, "blackBodyInit");
            res.sample = tracer.buildProgram(linkable, "blackBodySample");
            res.evaluate = tracer.buildProgram(linkable, "blackBodyEvaluate");
            res.pdf = tracer.buildProgram(linkable, "blackBodyPdf");
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
        SharedPtr<Config> mTexture;

    public:
        Matte(PiperContext& context, const SharedPtr<Config>& config, String path)
            : Surface(context), mKernelPath(std::move(path)), mTexture(config->at("Diffuse")) {}
        SurfaceProgram materialize(Tracer& tracer, ResourceHolder& holder, const CallSiteRegister& registerCall) const override {
            SurfaceProgram res;
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable =
                PIPER_FUTURE_CALL(pitu, generateLinkable)(tracer.getAccelerator().getSupportedLinkableFormat()).getSync();
            res.init = tracer.buildProgram(linkable, "matteInit");
            res.sample = tracer.buildProgram(linkable, "matteSample");
            res.evaluate = tracer.buildProgram(linkable, "matteEvaluate");
            res.pdf = tracer.buildProgram(linkable, "mattePdf");

            const auto texture = tracer.generateTexture(mTexture, 4);
            auto [SBT, prog] = texture->materialize(tracer, holder, registerCall);

            const MatteData data{ registerCall(prog, SBT) };
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
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
