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
#include "../STL/SharedPtr.hpp"
#include "../STL/UniquePtr.hpp"
// forward declaration
#include "Forward.hpp"

namespace Piper {
    class Uncopyable {
    public:
        Uncopyable() = default;
        Uncopyable(const Uncopyable& rhs) = delete;
        Uncopyable(Uncopyable&& rhs) = default;
        Uncopyable& operator=(const Uncopyable& rhs) = delete;
        Uncopyable& operator=(Uncopyable&& rhs) = default;
    };

    class Unmovable {
    public:
        Unmovable() = default;
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
        virtual ~Object() = default;
    };

#define PIPER_INTERFACE_CONSTRUCT(NAME, FATHER) \
    explicit NAME(PiperContext& context) noexcept : FATHER(context) {}

    template <typename T, typename... Args>
    auto makeSharedObject(PiperContext& context, Args&&... args) {
        auto& allocator = context.getAllocator();
        return makeSharedPtr<T>(allocator, context, std::forward<Args>(args)...);
    }
    template <typename T>
    using UniqueObject = UniquePtr<T, DefaultDeleter<Object>>;
    template <typename Base, typename T, typename... Args>
    auto makeUniqueObject(PiperContext& context, Args&&... args) {
        STLAllocator allocator = context.getAllocator();
        auto ptr = reinterpret_cast<T*>(allocator.allocate(sizeof(T)));
        new(ptr) T(context, std::forward<Args>(args)...);
        return UniqueObject<Base>{ ptr, DefaultDeleter<Object>{ allocator } };
    }

}  // namespace Piper
