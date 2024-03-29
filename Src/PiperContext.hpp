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
#include "Interface/Object.hpp"
#include "PiperAPI.hpp"

namespace Piper {
    class PiperContext : private Unmovable {  // NOLINT(cppcoreguidelines-special-member-functions)
    public:
        virtual Logger& getLogger() noexcept = 0;
        virtual ModuleLoader& getModuleLoader() noexcept = 0;
        virtual Scheduler& getScheduler() noexcept = 0;
        virtual FileSystem& getFileSystem() noexcept = 0;
        virtual Allocator& getAllocator() noexcept = 0;
        virtual UnitManager& getUnitManager() noexcept = 0;
        virtual ErrorHandler& getErrorHandler() noexcept = 0;
        virtual PITUManager& getPITUManager() noexcept = 0;
        //TODO:available for components?
        virtual bool complete() noexcept = 0;

        // TODO:better Interface?
        virtual void notify(FutureImpl* event) = 0;

        virtual ~PiperContext() = default;
    };

    class PiperContextOwner : public PiperContext {
    public:
        virtual void setLogger(SharedPtr<Logger> logger) noexcept = 0;
        virtual void setScheduler(SharedPtr<Scheduler> scheduler) noexcept = 0;
        virtual void setFileSystem(SharedPtr<FileSystem> filesystem) noexcept = 0;
        virtual void setAllocator(SharedPtr<Allocator> allocator) noexcept = 0;
        virtual void setPITUManager(SharedPtr<PITUManager> manager) noexcept = 0;
    };
}  // namespace Piper

PIPER_API Piper::PiperContextOwner* piperCreateContext();
PIPER_API void piperDestroyContext(Piper::PiperContextOwner* context);
