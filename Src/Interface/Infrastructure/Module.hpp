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

#pragma once
#include "../../STL/GSL.hpp"
#include "../../STL/StringView.hpp"
#include "../Object.hpp"
#include "Concurrency.hpp"
#include "Config.hpp"

namespace Piper {
    class Config;

    class Module : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Module, Object)
        virtual Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                         const Future<void>& module) = 0;
        virtual ~Module() = default;
    };

    class ModuleLoader : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleLoader, Object)
        virtual Future<void> loadModule(const SharedObject<Config>& moduleDesc, const String& descPath) = 0;
        virtual Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                         const Future<void>& module) = 0;

        virtual Future<void> loadModule(const String& moduleID) = 0;
        virtual Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config) = 0;
        virtual void addModuleDescription(const SharedObject<Config>& moduleDesc, const String& descPath) = 0;
        virtual ~ModuleLoader() = default;
    };
}  // namespace Piper

#define PIPER_INIT_MODULE_IMPL(CLASS)                                                                                \
    extern "C" PIPER_API Piper::Module* piperInitModule(Piper::PiperContext& context, Piper::Allocator& allocator) { \
        struct Deleter {                                                                                             \
            Piper::Allocator& allocator;                                                                             \
            void operator()(CLASS* ptr) const {                                                                      \
                allocator.free(reinterpret_cast<Piper::Ptr>(ptr));                                                   \
            }                                                                                                        \
        };                                                                                                           \
        Piper::UniquePtr<CLASS, Deleter> ptr = { reinterpret_cast<CLASS*>(allocator.alloc(sizeof(CLASS))),           \
                                                 Deleter{ allocator } };                                             \
        new(ptr.get()) CLASS(context);                                                                               \
        return ptr.release();                                                                                        \
    }                                                                                                                \
    extern "C" PIPER_API const char* piperGetCompatibilityFeature() {                                                \
        return PIPER_ABI "@" PIPER_STL "@" PIPER_INTERFACE;                                                          \
    }
\
