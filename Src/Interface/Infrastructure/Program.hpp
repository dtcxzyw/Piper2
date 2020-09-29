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
#include "../ContextResource.hpp"

namespace Piper {

    class PlatformIndependentTranslationUnit : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(PlatformIndependentTranslationUnit, Object)
        virtual ~PlatformIndependentTranslationUnit() = 0{}
    };

    class LinkableProgram : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(LinkableProgram, ContextResource)
        virtual ~LinkableProgram() = 0{}
    };

    // entry,resource binding
    class RunnableProgram : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(RunnableProgram, ContextResource)
        virtual ~RunnableProgram() = 0{}
    };

    class Accelerator : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, ContextResource)
        virtual ~Accelerator() = 0 {}
    };
}  // namespace Piper
