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
#include "../PiperAPI.hpp"
#define EASTL_USER_DEFINED_ALLOCATOR
#define EASTLAllocatorType Piper::STLAllocator
// TODO:throw exception
#define EASTLAllocatorDefault() nullptr
#include "GSL.hpp"
#include <eastl/internal/config.h>
#pragma warning(pop)

namespace Piper {
    class Allocator;

    // TODO:alignment
    class PIPER_API STLAllocator {
    private:
        Allocator* mAllocator;

    public:
        STLAllocator(Allocator& allocator) : mAllocator(&allocator) {}
        explicit STLAllocator(const char* = EASTL_NAME_VAL(EASTL_ALLOCATOR_DEFAULT_NAME)) : mAllocator(nullptr) {}
        STLAllocator(const STLAllocator& x, const char*) : STLAllocator(x) {}

        void* allocate(size_t n, int flags = 0);
        void* allocate(size_t n, size_t alignment, size_t offset, int flags = 0);
        void deallocate(void* p, size_t n) noexcept;

        const char* get_name() const noexcept {
            return "Piper Allocator";
        }
        void set_name(const char*) noexcept {}
        bool operator!=(const STLAllocator& rhs) const noexcept {
            return &mAllocator != &rhs.mAllocator;
        }
    };

}  // namespace Piper

#include "EASTL/allocator.h"
