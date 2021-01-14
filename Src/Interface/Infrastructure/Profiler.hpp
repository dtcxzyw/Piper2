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
#include "../../STL/String.hpp"
#include "../Object.hpp"

namespace Piper {
    // TODO:better interface for tracer and other components
    enum class StatisticsType { Bool, UInt, Float, Time };
    class Profiler : public Object {  // NOLINT(cppcoreguidelines-special-member-functions)
    public:
        PIPER_INTERFACE_CONSTRUCT(Profiler, Object)
        virtual StatisticsHandle registerDesc(StringView group, StringView name, const void* uid, StatisticsType type,
                                              uint32_t maxValue = 0) = 0;
        [[nodiscard]] virtual String generateReport() const = 0;
        virtual ~Profiler() = default;
    };
}  // namespace Piper
