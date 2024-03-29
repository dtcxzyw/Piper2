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

#pragma once
#include "../../STL/GSL.hpp"
#include "../../STL/StringView.hpp"
#include "../Object.hpp"
#include "Concurrency.hpp"
#include "Config.hpp"

namespace Piper {
    class Module : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Module, Object)
        virtual Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                                      const Future<void>& module) = 0;
        virtual ~Module() = default;
    };

    class ModuleLoader : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleLoader, Object)
        virtual Future<void> loadModule(const SharedPtr<Config>& moduleDesc, const String& descPath) = 0;
        virtual Future<SharedPtr<Object>> newInstance(const String& classID, const SharedPtr<Config>& config,
                                                      const Future<void>& module) = 0;
        virtual Future<SharedPtr<Object>> newInstance(const SharedPtr<Config>& config) = 0;
        virtual Future<SharedPtr<Object>> newInstance(const StringView& classID) = 0;

        template <typename T, typename... Args>
        Future<SharedPtr<T>> newInstanceT(Args&&... args) {
            return dynamicSharedPtrCast<T>(newInstance(std::forward<Args>(args)...));
        }

        virtual Future<void> loadModule(const String& moduleID) = 0;
        virtual void addModuleDescription(const SharedPtr<Config>& moduleDesc, const String& descPath) = 0;
        virtual ~ModuleLoader() = default;
    };
}  // namespace Piper

#define PIPER_INIT_MODULE_IMPL(CLASS)                                                                              \
    extern "C" PIPER_API Piper::Module* piperInitModule(Piper::PiperContext& context, Piper::Allocator& allocator, \
                                                        Piper::CString path) {                                     \
        struct Deleter {                                                                                           \
            Piper::Allocator& allocator;                                                                           \
            void operator()(CLASS* ptr) const {                                                                    \
                allocator.free(reinterpret_cast<Piper::Ptr>(ptr));                                                 \
            }                                                                                                      \
        };                                                                                                         \
        Piper::UniquePtr<CLASS, Deleter> ptr = { reinterpret_cast<CLASS*>(allocator.alloc(sizeof(CLASS))),         \
                                                 Deleter{ allocator } };                                           \
        new(ptr.get()) CLASS(context, path);                                                                       \
        return ptr.release();                                                                                      \
    }                                                                                                              \
    extern "C" PIPER_API const char* piperGetProtocol() {                                                          \
        return PIPER_ABI "@" PIPER_STL "@" PIPER_INTERFACE;                                                        \
    }

// TODO:hook operator new/delete
