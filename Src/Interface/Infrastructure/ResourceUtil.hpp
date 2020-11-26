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

#include "../../STL/Function.hpp"
#include "../../STL/UMap.hpp"
#include "../Object.hpp"

namespace Piper {
    class PIPER_API ResourceHolder final : public Object {
    private:
        DynamicArray<SharedPtr<Object>> mPool;

    public:
        explicit ResourceHolder(PiperContext& context);
        void retain(SharedPtr<Object> ref);
    };

    using ResourceID = uint64_t;

    // TODO:thread safe?
    // TODO:concurrency
    class PIPER_API ResourceCacheManager final : public Object {
    private:
        // TODO:use Any?
        // TODO:use WeakPtr?
        UMap<ResourceID, SharedPtr<Object>> mCache;
        SharedPtr<Object> lookupImpl(ResourceID id) const;
        void reserve(ResourceID id, const SharedPtr<Object>& cache);

    public:
        explicit ResourceCacheManager(PiperContext& context);
        template <typename T>
        SharedPtr<T> materialize(ResourceID id, const Function<SharedPtr<T>>& gen) {
            auto cache = lookup<T>(id);
            if(cache)
                return cache;
            auto res = gen();
            reserve(id, res);
            return res;
        }
        template <typename T>
        SharedPtr<T> lookup(ResourceID id) const {
            auto res = lookupImpl(id);
            return res ? eastl::dynamic_shared_pointer_cast<T>(res) : nullptr;
        }
    };
}  // namespace Piper
