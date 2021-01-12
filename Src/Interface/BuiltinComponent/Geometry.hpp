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
    struct GeometryProgram final {
        SBTPayload payload;
        SharedPtr<RTProgram> intersect;
        SharedPtr<RTProgram> occlude;
        SharedPtr<RTProgram> surface;
    };

    struct SampledGeometryProgram final {
        SBTPayload payload;
        SharedPtr<RTProgram> sample;
    };

    class Geometry : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Geometry, Object);
        virtual ~Geometry() = default;
        [[nodiscard]] virtual Area<float> area() const = 0;
        [[nodiscard]] virtual AccelerationStructure& getAcceleration(Tracer& tracer) const = 0;
        [[nodiscard]] virtual GeometryProgram materialize(const MaterializeContext& ctx) const = 0;
        [[nodiscard]] virtual SampledGeometryProgram materialize(TraversalHandle traversal,
                                                                 const MaterializeContext& ctx) const = 0;
    };
}  // namespace Piper
