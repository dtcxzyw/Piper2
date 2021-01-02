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
        virtual LightSamplerProgram materialize(const MaterializeContext& context) const = 0;
    };
    struct LightProgram final {
        SharedPtr<RTProgram> init;
        SharedPtr<RTProgram> sample;
        SharedPtr<RTProgram> evaluate;
        SharedPtr<RTProgram> pdf;
        SBTPayload payload;
    };
    // TODO:Light Instance?
    class Light : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Light, Object)
        virtual ~Light() = default;
        virtual LightProgram materialize(const MaterializeContext& ctx) const = 0;
        virtual bool isDelta() const noexcept = 0;
    };
}  // namespace Piper
