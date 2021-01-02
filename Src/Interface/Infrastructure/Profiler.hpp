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
#include "../Object.hpp"

namespace Piper {
    enum class StatisticsType { Bool, UInt, Float, Time };
    class Profiler : public Object {  // NOLINT(cppcoreguidelines-special-member-functions)
    public:
        PIPER_INTERFACE_CONSTRUCT(Profiler, Object)
        virtual uint32_t registerDesc(StringView group, StringView name, const void* uid, StatisticsType type) = 0;
        [[nodiscard]] virtual String generateReport() const = 0;
        virtual ~Profiler() = default;
    };
}  // namespace Piper
