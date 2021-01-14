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
    struct LightSamplerProgram final {
        SharedPtr<RTProgram> select;
        SBTPayload payload;
    };
    class LightSampler : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(LightSampler, Object)
        virtual ~LightSampler() = default;
        // TODO:state less
        virtual void preprocess(const Span<const SharedPtr<Light>>& lights) = 0;
        [[nodiscard]] virtual LightSamplerProgram materialize(const MaterializeContext& context) const = 0;
    };
    struct LightProgram final {
        SharedPtr<RTProgram> init;
        SharedPtr<RTProgram> sample;
        SharedPtr<RTProgram> evaluate;
        SharedPtr<RTProgram> pdf;
        SBTPayload payload;
    };

    enum class LightAttributes : uint32_t { Delta = 1, Infinite = 2, Area = 4 };
    constexpr bool match(LightAttributes provide, LightAttributes require) {
        return (static_cast<uint32_t>(provide) & static_cast<uint32_t>(require)) == static_cast<uint32_t>(provide);
    }
    constexpr LightAttributes operator|(LightAttributes a, LightAttributes b) {
        return static_cast<LightAttributes>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    class Light : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Light, Object)
        virtual ~Light() = default;
        [[nodiscard]] virtual LightProgram materialize(TraversalHandle traversal, const MaterializeContext& ctx) const = 0;
        [[nodiscard]] virtual const Geometry* getGeometry() const {
            return nullptr;
        }
        [[nodiscard]] virtual LightAttributes attributes() const noexcept = 0;
    };
}  // namespace Piper
