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
#include "../../Kernel/Protocol.hpp"
#include "Tracer.hpp"

namespace Piper {
    struct FilterProgram final {
        SBTPayload payload;
        SharedPtr<RTProgram> weight;
    };

    class Filter : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Filter, Object)
        virtual ~Filter() = default;
        [[nodiscard]] virtual FilterProgram materialize(const MaterializeContext& ctx) const = 0;
    };

    struct RenderDriverProgram final {
        SBTPayload payload;
        SharedPtr<RTProgram> accumulate;
    };
    class RenderDriver : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RenderDriver, Object)
        virtual ~RenderDriver() = default;
        virtual void renderFrame(DynamicArray<Spectrum<Radiance>>& res, uint32_t width, uint32_t height, const RenderRECT& rect,
                                 const SensorNDCAffineTransform& transform, Tracer& tracer, TraceLauncher& launcher) = 0;
        [[nodiscard]] virtual RenderDriverProgram materialize(const MaterializeContext& ctx) const = 0;
    };
}  // namespace Piper
