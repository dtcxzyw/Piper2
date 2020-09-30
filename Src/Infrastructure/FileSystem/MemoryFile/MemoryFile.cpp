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
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include <new>

namespace Piper {
    class MemoryFile final : public FileSystem {
    private:
    public:
        PIPER_INTERFACE_CONSTRUCT(MemoryFile, FileSystem)
        void removeFile(const StringView& path) override {
            throw;
        }
        String findFile(const StringView& path, const Span<StringView>& searchDirs) override {
            throw;
        }
        void createDir(const StringView& path) override {
            throw;
        }
        void removeDir(const StringView& path) override {
            throw;
        }
        String findDir(const StringView& path, const Span<StringView>& searchDirs) override {
            throw;
        }

        bool exist(const StringView& path) override {
            throw;
        }
        Permission permission(const StringView& path) override {
            throw;
        }
    };
    class ModuleImpl final : public Module {
    private:
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                 const Future<void>& module) override {
            if(classID == "MemoryFile") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<MemoryFile>(context())));
            }
            throw;
        }
    };
}  // namespace Piper

extern "C" PIPER_API Piper::Module* initModule(Piper::PiperContext& context, Piper::Allocator& allocator) {
    struct Deleter {
        Piper::Allocator& allocator;
        void operator()(Piper::ModuleImpl* ptr) const {
            allocator.free(reinterpret_cast<Piper::Ptr>(ptr));
        }
    };
    std::unique_ptr<Piper::ModuleImpl, Deleter> ptr = {
        reinterpret_cast<Piper::ModuleImpl*>(allocator.alloc(sizeof(Piper::ModuleImpl))), Deleter{ allocator }
    };
    new(ptr.get()) Piper::ModuleImpl(context);
    return ptr.release();
}
