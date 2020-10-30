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
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/PerformancePrimitivesLibrary.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <map>
#include <new>
#include <utility>

namespace Piper {
    // TODO:tracing
    // TODO:FP Flag
    class Float final : public FloatingPointLibrary {
    private:
        Instruction mInstruction;
        String mName;
        String mModulePath;

    public:
        Float(PiperContext& context, const SharedPtr<Config>& config, String path)
            : FloatingPointLibrary(context), mModulePath(std::move(path)) {
            mName = config->at("Name")->get<String>();
            auto inst = config->at("Instruction")->get<String>();
            if(inst == "Float32")
                mInstruction = Instruction::Float32;
            else
                context.getErrorHandler().raiseException("Unsupported floating point type", PIPER_SOURCE_LOCATION());
        }
        Instruction instruction() const noexcept override {
            return mInstruction;
        }
        size_t elementAlignment() const noexcept override {
            return alignof(float);
        }
        size_t elementSize() const noexcept override {
            return sizeof(float);
        }
        String typeName() const override {
            return mName;
        }
        Future<SharedPtr<PITU>> generateLinkable(const SharedPtr<PITUManager>& manager) const override {
            auto source = manager->loadPITU(mModulePath + "/Kernel.bc");
            return manager->appendSuffix(source, mName);
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mModulePath;

    public:
        ModuleImpl(PiperContext& context, const char* path) : Module(context), mModulePath(path, context.getAllocator()) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Float") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Float>(context(), config, mModulePath)));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
