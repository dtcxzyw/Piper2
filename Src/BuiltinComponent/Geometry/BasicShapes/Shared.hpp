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
#include "../../../Kernel/Protocol.hpp"

namespace Piper {
    struct PerPlaneData final {
        Point<Distance, FOR::Local> origin;
        Vector<Distance, FOR::Local> u, v;
        Normal<float, FOR::Local> normal;
        Normal<float, FOR::Local> tangent;
        uint32_t maxDetComp;
    };
    struct PlaneData final {
        uint32_t primitives;
    };
    struct CDFData final {
        uint32_t primitives;
        TraversalHandle traversal;
        uint32_t cdf;
        uint32_t pdf;
        Dimensionless<float> inverseArea;
        uint32_t size;
    };
}  // namespace Piper
