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
    class RunnableProgramUntyped : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RunnableProgramUntyped, Object)
        // TODO:better interface
        // TODO:interface check
        virtual void* lookup(const String& symbol) = 0;
    };

    template <typename... Args>
    class RunnableProgram final {
    private:
        Future<SharedPtr<RunnableProgramUntyped>> mProgram;
        static_assert((std::is_trivial_v<Args> && ...));

    public:
        explicit RunnableProgram(Future<SharedPtr<RunnableProgramUntyped>> program) : mProgram(std::move(program)) {}
        [[nodiscard]] auto get() const {
            return mProgram;
        }
    };

    struct ContextHandleReserved;
    using ContextHandle = const ContextHandleReserved*;  // Manager of resources and command queues
    struct CommandQueueHandleReserved;
    using CommandQueueHandle = const CommandQueueHandleReserved*;  // Async engine

    class ResourceInstance : public Object {
    private:
        SharedPtr<FutureImpl> mFuture;

    public:
        PIPER_INTERFACE_CONSTRUCT(ResourceInstance, Object);
        [[nodiscard]] virtual ResourceHandle getHandle() const noexcept = 0;
        void setFuture(SharedPtr<FutureImpl> future) {
            if(mFuture && mFuture.unique() && !mFuture->ready())
                context().getErrorHandler().raiseException("Not handled future", PIPER_SOURCE_LOCATION());
            mFuture = std::move(future);
        }
        [[nodiscard]] const SharedPtr<FutureImpl>& getFuture() const {
            return mFuture;
        }
    };

    enum class ResourceShareMode { Unique, Sharable };

    // TODO: type check?
    // TODO: move implementation to PiperCore
    // TODO: support demand-loaded resource
    // Just copy
    class Resource : public Object {
    private:
        UMap<ContextHandle, SharedPtr<ResourceInstance>> mReferences;
        mutable Pair<ContextHandle, SharedPtr<ResourceInstance>> mMainInstance;
        // lazy allocation
        // TODO: lock-free
        mutable std::mutex mMainMutex;
        std::shared_mutex mReferenceMutex;

    protected:
        using Instance = decltype(mMainInstance);
        // TODO: use map-reduce
        // write to main -> read from copies
        virtual void flushBeforeRead(const Instance& dest) const = 0;
        [[nodiscard]] virtual Instance instantiateMain() const = 0;
        [[nodiscard]] virtual Instance instantiateReference(ContextHandle ctx) const = 0;

        Future<void> syncToReferences() {
            auto& scheduler = context().getScheduler();
            if(getShareMode() == ResourceShareMode::Unique) {
                std::shared_lock<std::shared_mutex> guard{ mReferenceMutex };
                DynamicArray<Future<void>> futures{ context().getAllocator() };
                for(auto&& inst : mReferences) {
                    flushBeforeRead(inst);
                    futures.push_back(Future<void>{ inst.second->getFuture() });
                }
                return scheduler.wrap(futures);
            }
            return scheduler.ready();
        }

        [[nodiscard]] auto& requireMain() const {
            if(!mMainInstance.second) {
                std::lock_guard<std::mutex> guard{ mMainMutex };
                // double check
                if(!mMainInstance.second)
                    mMainInstance = instantiateMain();
            }
            return mMainInstance;
        }

        [[nodiscard]] auto& requireReference(const ContextHandle ctx) {
            {
                std::shared_lock<std::shared_mutex> guard{ mReferenceMutex };
                const auto iter = mReferences.find(ctx);
                if(iter != mReferences.cend())
                    return *iter;
            }
            std::lock_guard<std::shared_mutex> guard{ mReferenceMutex };
            auto&& inst = *mReferences.insert(instantiateReference(ctx)).first;
            flushBeforeRead(inst);
            return inst;
        }

        // reserved for map-reduce
        // write to instances -> read from main
        // virtual void flushAfterWrite()=0;

    public:
        explicit Resource(PiperContext& context) : Object{ context }, mReferences{ context.getAllocator() } {}
        [[nodiscard]] Future<void> available() const {
            return Future<void>{ requireMain().second->getFuture() };
        }
        [[nodiscard]] virtual ResourceShareMode getShareMode() const noexcept = 0;
        [[nodiscard]] const SharedPtr<ResourceInstance>& require(const ContextHandle ctx) {
            auto&& main = requireMain();
            if(getShareMode() == ResourceShareMode::Sharable || main.first == ctx)
                return main.second;
            return requireReference(ctx).second;
        }
        [[nodiscard]] SharedPtr<FutureImpl> access() {
            return syncToReferences().raw();
        }
        void makeDirty(const SharedPtr<FutureImpl>& future) const {
            requireMain().second->setFuture(future);
        }
    };

    enum class ResourceAccessMode { ReadOnly, ReadWrite };
    // reserved for map-reduce
    // enum class ResourceAccessLocality { Random, Local };

    struct ResourceView final {
        SharedPtr<Resource> resource;
        ResourceAccessMode accessMode;

        // TODO: divide resource for Multi-GPU
        // ResourceAccessLocality accessLocality;
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

    class Buffer : public Resource {
    public:
        Buffer(PiperContext& context) : Resource{ context } {}
        [[nodiscard]] virtual size_t size() const noexcept = 0;

        // For CPU: reduce copy
        // For CUDA: use pinned memory
        virtual void upload(Function<void, Ptr> prepare) = 0;

        virtual void reset() = 0;
        // TODO: move ownership on CPU to reduce copy
        [[nodiscard]] virtual Future<Binary> download() const = 0;
    };

    // TODO: Allocator per command queue
    class Accelerator : public Object {
    private:
        [[nodiscard]] virtual Future<void> launchKernelImpl(const Dim3& grid, const Dim3& block,
                                                            const Future<SharedPtr<RunnableProgramUntyped>>& kernel,
                                                            const DynamicArray<ResourceView>& resources,
                                                            const ArgumentPackage& args) = 0;
        [[nodiscard]] virtual Future<SharedPtr<RunnableProgramUntyped>> compileKernelImpl(const Span<LinkableProgram>& linkable,
                                                                                          const String& entry) = 0;

    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, Object);
        [[nodiscard]] virtual CString getNativePlatform() const noexcept = 0;
        [[nodiscard]] virtual Span<const CString> getSupportedLinkableFormat() const noexcept = 0;

        template <typename... Args>
        [[nodiscard]] RunnableProgram<Args...> compileKernel(const Span<LinkableProgram>& linkable, const String& entry) {
            return RunnableProgram<Args...>{ compileKernelImpl(linkable, entry) };
        }
        template <typename... Args>
        [[nodiscard]] Future<void> launchKernel(const Dim3& grid, const Dim3& block, const RunnableProgram<Args...>& kernel,
                                                const DynamicArray<ResourceView>& resources, const Args&... args) {
            return launchKernelImpl(grid, block, kernel.get(), resources, ArgumentPackage{ context(), args... });
        }

        // TODO: better interface
        // virtual void applyNative(Function<void, ContextHandle, CommandQueueHandle> func,
        //                         DynamicArray<SharedPtr<ResourceView>> resources) = 0;

        // TODO:support DMA? (DMA may cause bad performance)
        [[nodiscard]] virtual SharedPtr<Buffer> createBuffer(size_t size, size_t alignment) = 0;
    };
}  // namespace Piper
