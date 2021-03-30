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
#include "Tracer.hpp"

namespace Piper {
    struct TextureProgram final {
        SBTPayload payload;
        SharedPtr<RTProgram> sample;
    };

    // TODO:load on demand
    // TODO:2D distribution support
    class Texture : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Texture, Object);
        [[nodiscard]] virtual uint32_t channel() const noexcept = 0;
        virtual TextureProgram materialize(const MaterializeContext& ctx) const = 0;
    };
    // TODO:mipmap and anisotropic interpolation
    class TextureSampler : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(TextureSampler, Object);
        [[nodiscard]] virtual SharedPtr<Texture> generateTexture(const SharedPtr<Image>& image, TextureWrap wrap) const = 0;
    };
    // TODO:texCoord modifier

}  // namespace Piper
