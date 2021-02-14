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
        PIPER_INTERFACE_CONSTRUCT(Sensor, Object);
        virtual SensorProgram materialize(TraversalHandle traversal, const MaterializeContext& ctx) const = 0;
        [[nodiscard]] virtual float aspectRatio() const noexcept = 0;
    };

    // TODO: move to PiperCore
    // Helper function
    enum class FitMode : uint32_t { Fill, OverScan };

    [[nodiscard]] inline Pair<SensorNDCAffineTransform, RenderRECT>
    calcRenderRECT(const uint32_t width, const uint32_t height, const float deviceAspectRatio, const FitMode fitMode) {
        RenderRECT rect;
        SensorNDCAffineTransform transform;
        const auto imageAspectRatio = static_cast<float>(width) / static_cast<float>(height);
        const auto iiar = 1.0f / imageAspectRatio;
        const auto idar = 1.0f / deviceAspectRatio;
        if(fitMode == FitMode::Fill) {
            rect = { 0, 0, width, height };
            if(imageAspectRatio > deviceAspectRatio) {
                transform = { 0.0f, (idar - iiar) * 0.5f * deviceAspectRatio, 1.0f, iiar * deviceAspectRatio };
            } else {
                transform = { (deviceAspectRatio - imageAspectRatio) * 0.5f * idar, 0.0f, imageAspectRatio * idar, 1.0f };
            }
        } else {
            if(imageAspectRatio > deviceAspectRatio) {
                transform = { -(imageAspectRatio - deviceAspectRatio) * 0.5f * idar, 0.0f, imageAspectRatio * idar, 1.0f };
                rect = { static_cast<uint32_t>(floorf(
                             std::max(0.0f, static_cast<float>(width) * (imageAspectRatio - deviceAspectRatio) * 0.5f * iiar))),
                         0,
                         std::min(width,
                                  static_cast<uint32_t>(
                                      ceilf(static_cast<float>(width) * (imageAspectRatio + deviceAspectRatio) * 0.5f * iiar))),
                         height };
                rect.width -= rect.left;
            } else {
                transform = { 0.0f, -(iiar - idar) * 0.5f * deviceAspectRatio, 1.0f, deviceAspectRatio * iiar };
                rect = { 0,
                         static_cast<uint32_t>(
                             floorf(std::max(0.0f, static_cast<float>(height) * (iiar - idar) * 0.5f * imageAspectRatio))),
                         width,
                         static_cast<uint32_t>(ceilf(static_cast<float>(height) * (iiar + idar) * 0.5f * imageAspectRatio)) };
                rect.height -= rect.top;
            }
        }
        return { transform, rect };
    }
}  // namespace Piper
