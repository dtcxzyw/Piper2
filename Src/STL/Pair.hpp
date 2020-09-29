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
#include <EASTL/utility.h>

namespace Piper {
    template <typename T1, typename T2>
    using Pair = eastl::pair<T1, T2>;

    template <typename T1, typename T2>
    constexpr inline auto makePair(T1&& a, T2&& b) {
        using T1Type = typename eastl::remove_reference_wrapper<typename eastl::decay<T1>::type>::type;
        using T2Type = typename eastl::remove_reference_wrapper<typename eastl::decay<T2>::type>::type;

        return eastl::pair<T1Type, T2Type>(eastl::forward<T1>(a), eastl::forward<T2>(b));
    }

}  // namespace Piper
