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
#include "Interface/Object.hpp"
#include "PiperAPI.hpp"

namespace Piper {
    class FutureImpl;
    class Logger;
    class ModuleLoader;
    class Scheduler;
    class FileSystem;
    class Allocator;
    class UnitManager;
    class ErrorHandler;

    class PiperContext : private Unmovable {
    public:
        virtual Logger& getLogger() noexcept = 0;
        virtual ModuleLoader& getModuleLoader() noexcept = 0;
        virtual Scheduler& getScheduler() noexcept = 0;
        virtual FileSystem& getFileSystem() noexcept = 0;
        virtual Allocator& getAllocator() noexcept = 0;
        virtual UnitManager& getUnitManager() noexcept = 0;
        virtual ErrorHandler& getErrorHandler() noexcept = 0;

        // TODO:better Interface?
        virtual void notify(FutureImpl* event) = 0;

        virtual ~PiperContext() = default;
    };
    // TODO:stateless
    class PiperContextOwner : public PiperContext {
    public:
        virtual void setLogger(const SharedPtr<Logger>& logger) noexcept = 0;
        virtual void setScheduler(const SharedPtr<Scheduler>& scheduler) noexcept = 0;
        virtual void setFileSystem(const SharedPtr<FileSystem>& filesystem) noexcept = 0;
        virtual void setAllocator(const SharedPtr<Allocator>& allocator) noexcept = 0;
        virtual ~PiperContextOwner() = default;
    };
}  // namespace Piper

PIPER_API Piper::PiperContextOwner* piperCreateContext();
PIPER_API void piperDestroyContext(Piper::PiperContextOwner* context);
