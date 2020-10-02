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
#include <cstdint>
#include <new>

namespace Piper {

    using Ptr = uint64_t;

    /*
    class MemoryProvider : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(MemoryProvider, ContextResource)
        virtual Ptr alloc(const size_t size, const size_t align = 8) = 0;
        virtual void free(const Ptr ptr) = 0;
        virtual ~MemoryProvider() = 0 {}
        virtual bool isThreadSafety() const noexcept = 0;
    };
    */

    class Allocator : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(Allocator, ContextResource)
        // TODO:hardware_constructive_interference_size
        virtual Ptr alloc(const size_t size, const size_t align = 8) = 0;
        virtual void free(const Ptr ptr) = 0;
        virtual ~Allocator() = 0 {}
    };

    class MemoryArena : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(MemoryArena, ContextResource)
        virtual Ptr alloc(const size_t size, const size_t align = 8) = 0;
        virtual ~MemoryArena() = 0 {}
    };

}  // namespace Piper
