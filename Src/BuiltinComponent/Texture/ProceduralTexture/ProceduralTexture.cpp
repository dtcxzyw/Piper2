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
#include "../../../Interface/BuiltinComponent/Texture.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"

namespace Piper {
    class ConstantTexture final : public Texture {
    private:
        Data mData;
        String mKernelPath;

    public:
        ConstantTexture(PiperContext& context, const SharedPtr<Config>& config, const String& path)
            : Texture(context), mKernelPath(path + "/Kernel.bc") {
            const auto& elements = config->at("Value")->viewAsArray();
            mData.channel = static_cast<uint32_t>(elements.size());
            if(mData.channel != 1 && mData.channel != 2 && mData.channel != 4)
                context.getErrorHandler().raiseException("Unsupported channel " + toString(context.getAllocator(), mData.channel),
                                                         PIPER_SOURCE_LOCATION());
            for(uint32_t i = 0; i < mData.channel; ++i)
                mData.value[i].val = static_cast<float>(elements[i]->get<double>());
        }

        [[nodiscard]] uint32_t channel() const noexcept override {
            return mData.channel;
        }
        TextureProgram materialize(const MaterializeContext& ctx) const override {
            TextureProgram res;
            // TODO:concurrency
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            res.sample = ctx.tracer.buildProgram(
                PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.tracer.getAccelerator().getSupportedLinkableFormat()).getSync(),
                "constantTexture");
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
            if(classID == "ConstantTexture") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ConstantTexture>(context(), config, mPath)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
