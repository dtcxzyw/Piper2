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

    template <typename T, typename... Args>
    SharedPtr<T> makeSharedPtr(const STLAllocator& allocator, Args&&... args) {
        return eastl::allocate_shared<T>(allocator, eastl::forward<Args>(args)...);
    }
}  // namespace Piper
