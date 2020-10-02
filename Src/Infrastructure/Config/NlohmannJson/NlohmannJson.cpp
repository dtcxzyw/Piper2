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
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include <new>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

namespace Piper {
    class JsonSerializer final : public ConfigSerializer {
    public:
        PIPER_INTERFACE_CONSTRUCT(JsonSerializer, ConfigSerializer)
        SharedObject<Config> deserialize(const StringView& path) const override {}
        void serialize(const SharedObject<Config>& config, const StringView& path) const override {}
    };
    class ModuleImpl final : public Module {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                 const Future<void>& module) override {
            if(classID == "JsonSerializer") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<JsonSerializer>(context())));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
