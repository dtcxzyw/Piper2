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
#include "../../STL/DynamicArray.hpp"
#include "../../STL/String.hpp"
#include "../Object.hpp"
#include "Concurrency.hpp"

namespace Piper {
    class PITU;
    struct LinkableProgram final {
        Variant<Future<SharedPtr<PITU>>, Future<Binary>> exchange;
        String format;
        uint64_t UID;
    };

    // PlatformIndependentTranslationUnit
    class PITU : public Object {  // NOLINT(cppcoreguidelines-special-member-functions)
    public:
        PIPER_INTERFACE_CONSTRUCT(PITU, Object)
        // TODO: better interface
        [[nodiscard]] virtual LinkableProgram generateLinkable(const Span<const CString>& acceptableFormat) const = 0;
        [[nodiscard]] virtual String humanReadable() const = 0;
    };

    // TODO: Optimize
    class PITUManager : public Object {  // NOLINT(cppcoreguidelines-special-member-functions)
    public:
        PIPER_INTERFACE_CONSTRUCT(PITUManager, Object)
        [[nodiscard]] virtual Future<SharedPtr<PITU>> loadPITU(const String& path) const = 0;
        //[[nodiscard]] virtual Future<SharedPtr<PITU>> mergePITU(const DynamicArray<Future<SharedPtr<PITU>>>& pitus) const = 0;
        [[nodiscard]] virtual Future<SharedPtr<PITU>> linkPITU(const DynamicArray<Future<SharedPtr<PITU>>>& pitus,
                                                               UMap<String, String> staticRedirectedSymbols,
                                                               DynamicArray<String> dynamicSymbols) const = 0;
    };
}  // namespace Piper
