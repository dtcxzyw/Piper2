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
#include "../PiperAPI.hpp"
#include "../STL/SharedPtr.hpp"

namespace Piper {
    class Unmovable {
    public:
        Unmovable() = default;
        Unmovable(const Unmovable& rhs) = delete;
        Unmovable(Unmovable&& rhs) = delete;
        Unmovable& operator=(const Unmovable& rhs) = delete;
        Unmovable& operator=(Unmovable&& rhs) = delete;
    };

    class PiperContext;
    class Allocator;

    class Object : private Unmovable {
    private:
        PiperContext& mContext;

    public:
        explicit Object(PiperContext& context) noexcept : mContext(context) {}
        PiperContext& context() const noexcept {
            return mContext;
        }
        virtual ~Object() = 0 {}
    };

#define PIPER_INTERFACE_CONSTRUCT(NAME, FATHER) \
    explicit NAME(PiperContext& context) noexcept : FATHER(context) {}

    template <typename T>
    using SharedObject = eastl::shared_ptr<T>;

    class PIPER_API ObjectDeleter final {
    private:
        Allocator& mAllocator;

    public:
        explicit ObjectDeleter(Allocator& allocator) : mAllocator(allocator) {}
        void operator()(Object* obj);
    };

    template <typename T, typename... Args>
    auto makeSharedObject(PiperContext& context, Args&&... args) {
        auto& allocator = context.getAllocator();
        return makeSharedPtr<T>(allocator, ObjectDeleter{ allocator }, context, std::forward<Args>(args)...);
    }

    template <typename T>
    using UniqueObject = eastl::unique_ptr<T, ObjectDeleter>;
}  // namespace Piper
