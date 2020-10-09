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
#include "../Object.hpp"
#include <cstdint>
#include <new>

namespace Piper {

    using Ptr = uint64_t;

    /*
    class MemoryProvider : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MemoryProvider, Object)
        virtual Ptr alloc(const size_t size, const size_t align = alignof(max_align_t)) = 0;
        virtual void free(const Ptr ptr) = 0;
        virtual ~MemoryProvider() = default;
        virtual bool isThreadSafety() const noexcept = 0;
    };
    */

    class Allocator : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Allocator, Object)
        // TODO:hardware_constructive_interference_size
        // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0154r1.html
        virtual Ptr alloc(const size_t size, const size_t align = alignof(max_align_t)) = 0;
        virtual void free(const Ptr ptr) noexcept = 0;
        virtual ~Allocator() = default;
    };

    class MemoryArena : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MemoryArena, Object)
        virtual Ptr alloc(const size_t size, const size_t align = alignof(max_align_t)) = 0;
        virtual ~MemoryArena() = default;
    };

}  // namespace Piper
