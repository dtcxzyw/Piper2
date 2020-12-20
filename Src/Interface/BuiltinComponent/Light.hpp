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
    struct LightProgram final {
        SharedPtr<RTProgram> light;
        SBTPayload payload;
    };
    class Light : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Light, Object)
        virtual ~Light() = default;
        virtual LightProgram materialize(Tracer& tracer, ResourceHolder& holder) const = 0;
        virtual bool isDelta() const = 0;
    };
}  // namespace Piper
