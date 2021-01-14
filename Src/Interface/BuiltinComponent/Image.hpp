/*
   Copyright 2020-2021 Yingwei Zheng
   SPDX-License-Identifier: Apache-2.0

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
#include "../Object.hpp"

namespace Piper {
    struct ImageAttributes final {
        uint32_t width;
        uint32_t height;
        uint32_t channel;  // 1,2 or 4
    };
    // TODO:load on demand
    // TODO:multi-frame
    class Image : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Image, Object)
        virtual ~Image() = default;
        [[nodiscard]] virtual const ImageAttributes& attributes() const noexcept = 0;
        [[nodiscard]] virtual const std::byte* data() const noexcept = 0;  // unsigned char
    };
}  // namespace Piper
