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
    struct SensorProgram final {
        SBTPayload payload;
        SharedPtr<RTProgram> rayGen;
    };

    class Sensor : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Sensor, Object)
        virtual ~Sensor() = default;
        virtual SensorProgram materialize(TraversalHandle traversal, const MaterializeContext& ctx) const = 0;
        [[nodiscard]] virtual float aspectRatio() const noexcept = 0;
    };
}  // namespace Piper
