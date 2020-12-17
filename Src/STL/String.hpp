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
#pragma warning(push, 0)
#include "STLAllocator.hpp"
#define EASTL_EASTDC_API PIPER_API
#include <EASTL/string.h>
#pragma warning(pop)
#include <cinttypes>

namespace Piper {
    using String = eastl::u8string;

    namespace Detail {
        template <typename T>
        inline String toStringImpl(const STLAllocator& allocator, const char* format, const T value) {
            String res{ allocator };
            res.append_sprintf(format, value);
            return res;
        }
    }  // namespace Detail

    // TODO:use fmt
    inline String toString(const STLAllocator& allocator, int32_t value) {
        return Detail::toStringImpl(allocator, "%" PRId32, value);
    }
    inline String toString(const STLAllocator& allocator, int64_t value) {
        return Detail::toStringImpl(allocator, "%" PRId64, value);
    }
    inline String toString(const STLAllocator& allocator, uint32_t value) {
        return Detail::toStringImpl(allocator, "%" PRIu32, value);
    }
    inline String toString(const STLAllocator& allocator, uint64_t value) {
        return Detail::toStringImpl(allocator, "%" PRIu64, value);
    }
    inline String toString(const STLAllocator& allocator, float value) {
        return Detail::toStringImpl(allocator, "%f", value);
    }
    inline String toString(const STLAllocator& allocator, double value) {
        return Detail::toStringImpl(allocator, "%lf", value);
    }
    inline String toString(const STLAllocator& allocator, long double value) {
        return Detail::toStringImpl(allocator, "%Lf", value);
    }

    // TODO:parse
}  // namespace Piper
