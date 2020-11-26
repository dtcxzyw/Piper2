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

#include <gsl/gsl>

namespace Piper {

    // template <typename T>
    // using NotNull = gsl::not_null<T>;
    // TODO:rethink

    template <typename T>
    using Owner = gsl::owner<T>;

    // ES.107
    // using Index = gsl::index;
    using Index = std::size_t;

    template <typename T>
    using Span = gsl::span<T>;

    using CString = gsl::czstring<>;

}  // namespace Piper
