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
#include "../../Kernel/Protocol.hpp"
#include "../Infrastructure/Config.hpp"
#include "../Infrastructure/Logger.hpp"

namespace Piper {
    template <typename Float>
    Vector2<Float> parseVector2(const SharedPtr<Config>& config) {
        const auto& arr = config->viewAsArray();
        if(arr.size() != 2)
            config->context().getErrorHandler().raiseException("Dimension must be 2", PIPER_SOURCE_LOCATION());
        return { Float{ static_cast<float>(arr[0]->get<double>()) }, Float{ static_cast<float>(arr[1]->get<double>()) } };
    }

    template <typename Float, FOR ref>
    Vector<Float, ref> parseVector(const SharedPtr<Config>& config) {
        const auto& arr = config->viewAsArray();
        if(arr.size() != 3)
            config->context().getErrorHandler().raiseException("Dimension must be 3", PIPER_SOURCE_LOCATION());
        return { Float{ static_cast<float>(arr[0]->get<double>()) }, Float{ static_cast<float>(arr[1]->get<double>()) },
                 Float{ static_cast<float>(arr[2]->get<double>()) } };
    }
    template <typename Float, FOR ref>
    Point<Float, ref> parsePoint(const SharedPtr<Config>& config) {
        const auto& arr = config->viewAsArray();
        if(arr.size() != 3)
            config->context().getErrorHandler().raiseException("Dimension must be 3", PIPER_SOURCE_LOCATION());
        return Point<Float, ref>{ Float{ static_cast<float>(arr[0]->get<double>()) },
                                  Float{ static_cast<float>(arr[1]->get<double>()) },
                                  Float{ static_cast<float>(arr[2]->get<double>()) } };
    }
    template <typename Float>
    Spectrum<Float> parseSpectrum(const SharedPtr<Config>& config) {
        const auto& arr = config->viewAsArray();
        if(arr.size() != 3)
            config->context().getErrorHandler().raiseException("Dimension must be 3", PIPER_SOURCE_LOCATION());
        return Spectrum<Float>{ Float{ static_cast<float>(arr[0]->get<double>()) },
                                Float{ static_cast<float>(arr[1]->get<double>()) },
                                Float{ static_cast<float>(arr[2]->get<double>()) } };
    }
}  // namespace Piper
