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
#include "../../STL/String.hpp"
#include "../../STL/Vector.hpp"
#include "../ContextResource.hpp"
#include "Concurrency.hpp"

namespace Piper {
    // PlatformIndependentTranslationUnit
    // TODO:output
    class PITU : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(PITU, Object)
        virtual ~PITU() = default;
        virtual Future<Vector<std::byte>> generateLinkable(const Vector<CString>& acceptableFormat) const = 0;
    };

    // TODO:Optimize
    class PITUManager : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(PITUManager, Object);
        virtual ~PITUManager() = default;
        virtual Future<SharedObject<PITU>> loadPITU(const String& path) const = 0;
        virtual Future<SharedObject<PITU>> mergePITU(const Future<Vector<SharedObject<PITU>>>& pitus) const = 0;
    };

    class RunnableProgram : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(RunnableProgram, ContextResource)
        virtual ~RunnableProgram() = default;
    };
}  // namespace Piper
