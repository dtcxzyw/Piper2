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
#include "Object.hpp"
#include <cstdint>

namespace Piper {
    using ContextHandle = uint64_t;

    class ContextResource : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ContextResource, Object)
        virtual ContextHandle getContextHandle() const = 0;
        virtual ~ContextResource() = 0{}
    };

}  // namespace Piper
