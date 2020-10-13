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
#include "../../STL/Vector.hpp"
#include "../Object.hpp"
#include "Concurrency.hpp"

namespace Piper {
    // TODO:Stream Pipeline
    // TODO:separate I/O
    class Stream : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Stream, Object)
        virtual ~Stream() = default;
        virtual size_t size() const noexcept = 0;
        // TODO:FutureSequence/reduce copy
        virtual Future<Vector<std::byte>> read(const size_t offset, const size_t size) = 0;
        virtual Future<void> write(const size_t offset, const Future<Vector<std::byte>>& data) = 0;
    };

    class MappedSpan : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MappedSpan, Object)
        virtual Span<std::byte> get() const noexcept = 0;
    };

    class MappedMemory : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MappedMemory, Object)
        virtual size_t size() const noexcept = 0;
        virtual size_t alignment() const noexcept = 0;
        virtual SharedPtr<MappedSpan> map(const size_t offset, const size_t size) const = 0;
    };
}  // namespace Piper
