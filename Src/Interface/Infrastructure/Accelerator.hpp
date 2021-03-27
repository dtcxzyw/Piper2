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
#include "../../Kernel/DeviceRuntime.hpp"
#include "../../STL/DynamicArray.hpp"
#include "../../STL/Function.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/String.hpp"
#include "../../STL/UMap.hpp"
#include "../Object.hpp"
#include "Allocator.hpp"
#include "Concurrency.hpp"
#include <shared_mutex>

namespace Piper {

    struct ContextHandleReserved;
    using ContextHandle = ContextHandleReserved*;  // Manager of resources and command queues
    struct CommandQueueHandleReserved;
    using CommandQueueHandle = CommandQueueHandleReserved*;  // Async engine
    class ResourceInstance;

    class Context : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Context, Object);
        [[nodiscard]] virtual std::lock_guard<std::recursive_mutex> makeCurrent() = 0;
        [[nodiscard]] virtual ContextHandle getHandle() const noexcept = 0;
        [[nodiscard]] virtual CommandQueueHandle select() = 0;
        //[[nodiscard]] virtual SharedPtr<ResourceInstance> createContextSpecificBuffer(size_t size, size_t alignment) = 0;
    };

    class ResourceInstance : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ResourceInstance, Object);
        [[nodiscard]] virtual ResourceHandle getHandle() const noexcept = 0;
        [[nodiscard]] virtual SharedPtr<FutureImpl> getFuture() const noexcept = 0;
    };

    class Resource : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Resource, Object);
        // TODO: reduce copy?
        virtual SharedPtr<ResourceInstance> requireInstance(Context* ctx) = 0;
    };

    // helper function
    template <typename Callable>
    SharedPtr<ResourceInstance> safeRequireInstance(std::shared_mutex& mutex,
                                                    UMap<Context*, SharedPtr<ResourceInstance>>& instances, Context* ctx,
                                                    Callable&& init) {
        std::shared_lock<std::shared_mutex> guard{ mutex };
        const auto iter = instances.find(ctx);
        if(iter == instances.cend()) {
            // upgrade
            guard.unlock();
            std::lock_guard<std::shared_mutex> uniqueGuard{ mutex };
            // double check
            const auto trueIter = instances.find(ctx);
            if(trueIter != instances.cend())
                return trueIter->second;

            return instances.insert(makePair(ctx, init())).first->second;
        }
        return iter->second;
    }

    // TODO: type check?
    class TiledOutput : public Resource {
    public:
        PIPER_INTERFACE_CONSTRUCT(TiledOutput, Resource);
        // TODO: reduce copy
        [[nodiscard]] virtual Future<Binary> download() const = 0;
        // Future<SharedPtr<Resource>> asBuffer() const;
    };

    class Kernel : public Resource {
    public:
        PIPER_INTERFACE_CONSTRUCT(Kernel, Resource);
    };

    class ResourceLookUpTable : public Resource {
    public:
        PIPER_INTERFACE_CONSTRUCT(ResourceLookUpTable, Resource);
    };

    class ArgumentPackage final {
    private:
        // ReSharper disable once CppMemberFunctionMayBeStatic
        void append() {}

        template <typename T, typename... Args>
        std::enable_if_t<std::is_trivial_v<T>> append(const T& first, const Args&... args) {
            offset.push_back({ static_cast<uint32_t>(data.size()), static_cast<uint32_t>(sizeof(T)) });
            const auto begin = reinterpret_cast<Binary::const_pointer>(&first);
            const auto end = begin + sizeof(T);
            data.insert(data.cend(), begin, end);
            append(args...);
        }

    public:
        Binary data;
        DynamicArray<Pair<uint32_t, uint32_t>> offset;

        template <typename... Args>
        explicit ArgumentPackage(PiperContext& context, const Args&... args)
            : data{ context.getAllocator() }, offset{ context.getAllocator() } {
            append(args...);
        }
    };

    class SymbolBinding final : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(SymbolBinding, Object);
    };

    // TODO: tiled input?
    // TODO: resource allocation scheduler
    class Accelerator : public Object {
    private:
        [[nodiscard]] virtual Future<void> launchKernelImpl(const Dim3& grid, const Dim3& block, SharedPtr<Kernel> kernel,
                                                            SharedPtr<ResourceLookUpTable> root, ArgumentPackage args) = 0;

    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, Object);
        [[nodiscard]] virtual CString getNativePlatform() const noexcept = 0;
        [[nodiscard]] virtual Span<const CString> getSupportedLinkableFormat() const noexcept = 0;

        [[nodiscard]] virtual SharedPtr<Kernel> compileKernel(const Span<LinkableProgram>& linkable,
                                                              UMap<String, String> staticRedirectedSymbols,
                                                              DynamicArray<String> dynamicSymbols, String entryFunction) = 0;

        template <typename... Args>
        [[nodiscard]] Future<void> launchKernel(const Dim3& grid, const Dim3& block, SharedPtr<Kernel> kernel,
                                                SharedPtr<ResourceLookUpTable> root, const Args&... args) {
            static_assert((std::is_trivial_v<std::decay_t<Args>> && ...));
            return launchKernelImpl(grid, block, std::move(kernel), std::move(root), ArgumentPackage{ context(), args... });
        }

        // TODO: better interface
        // virtual void applyNative(Function<void, ContextHandle, CommandQueueHandle> func,
        //                         DynamicArray<SharedPtr<ResourceView>> resources) = 0;

        // TODO:support DMA? (DMA may cause bad performance)
        [[nodiscard]] virtual SharedPtr<Resource> createBuffer(size_t size, size_t alignment, Function<void, Ptr> data) = 0;

        // TODO: init/tiled/map/reduce
        [[nodiscard]] virtual SharedPtr<TiledOutput> createTiledOutput(size_t size, size_t alignment) = 0;

        [[nodiscard]] virtual SharedPtr<ResourceLookUpTable> createResourceLUT(DynamicArray<SharedPtr<Resource>> resources) = 0;
        [[nodiscard]] virtual const DynamicArray<Context*>& enumerateContexts() const = 0;
    };
}  // namespace Piper
