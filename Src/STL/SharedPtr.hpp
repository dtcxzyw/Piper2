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
#include "STLAllocator.hpp"
#include <EASTL/shared_ptr.h>

namespace Piper {
    template <typename T>
    using SharedPtr = eastl::shared_ptr<T>;

    template <typename T>
    struct DefaultDeleter final {
        STLAllocator allocator;
        void operator()(T* ptr) noexcept {
            ptr->~T();
            allocator.deallocate(ptr, 0);
        }
    };

    // TODO:make_shared
    template <typename T, typename... Args>
    SharedPtr<T> makeSharedPtr(STLAllocator allocator, Args&&... args) {
        auto ptr = reinterpret_cast<T*>(allocator.allocate(sizeof(T)));
        try {
            new(ptr) T(std::forward<Args>(args)...);
        } catch(...) {
            allocator.deallocate(ptr, sizeof(T));
            throw;
        }
        return SharedPtr<T>(ptr, DefaultDeleter<T>{ allocator }, allocator);
    }
}  // namespace Piper
