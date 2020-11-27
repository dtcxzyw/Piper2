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
#include "Tracer.hpp"

namespace Piper {
    struct MediumProgram final {
        SBTPayload payload;
    };

    class Medium : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Medium, Object)
        virtual ~Medium() = default;
        virtual MediumProgram materialize(Tracer& tracer, ResourceHolder& holder) const = 0;
    };
}  // namespace Piper