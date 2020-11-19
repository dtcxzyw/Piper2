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
#include "../../STL/DynamicArray.hpp"
#include "../../STL/String.hpp"
#include "../Object.hpp"
#include "Concurrency.hpp"

namespace Piper {
    struct LinkableProgram final {
        DynamicArray<std::byte> exchange;
        CString format;
    };

    // PlatformIndependentTranslationUnit
    class PITU : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(PITU, Object)
        virtual ~PITU() = default;
        virtual Future<LinkableProgram> generateLinkable(const Span<const CString>& acceptableFormat) const = 0;
        virtual String humanReadable() const = 0;
    };

    // TODO:Optimize
    // TODO:PPL
    class PITUManager : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(PITUManager, Object)
        virtual ~PITUManager() = default;
        virtual Future<SharedPtr<PITU>> loadPITU(const String& path) const = 0;
        virtual Future<SharedPtr<PITU>> mergePITU(const Future<DynamicArray<SharedPtr<PITU>>>& pitus) const = 0;
    };
}  // namespace Piper
