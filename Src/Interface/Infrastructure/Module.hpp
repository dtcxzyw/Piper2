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
        virtual ~Module() = 0 {}
    };

    class ModuleLoader : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleLoader, Object)
        virtual Future<void> loadModule(const SharedObject<Config>& packageDesc, const StringView& descPath) = 0;
        virtual Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                         const Future<void>& module) = 0;
        virtual ~ModuleLoader() = 0 {}
    };
}  // namespace Piper
