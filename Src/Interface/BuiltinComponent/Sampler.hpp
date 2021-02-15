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
    struct SamplerProgram final {
        SharedPtr<RTProgram> start;
        SharedPtr<RTProgram> generate;
    };

    struct SamplerAttributes final {
        SBTPayload payload;
        uint32_t maxDimension;
        uint32_t samplesPerPixel;
    };

    // TODO: support Trajectory Splitting
    class Sampler : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Sampler, Object);
        [[nodiscard]] virtual SamplerProgram materialize(const MaterializeContext& ctx) const = 0;
        [[nodiscard]] virtual SamplerAttributes generatePayload(uint32_t sampleWidth, uint32_t sampleHeight) const = 0;
    };
}  // namespace Piper
