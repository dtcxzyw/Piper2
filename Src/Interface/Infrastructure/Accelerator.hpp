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
#include "../../STL/DynamicArray.hpp"
#include "../../STL/Function.hpp"
#include "../../STL/String.hpp"
#include "../Object.hpp"
#include "Allocator.hpp"
#include "Concurrency.hpp"

namespace Piper {
    class RunnableProgram : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RunnableProgram, Object)
        virtual ~RunnableProgram() = default;
        // TODO:better interface
        virtual void* lookup(const String& symbol) = 0;
    };

    using CommandQueue = uint64_t;
    using Context = uint64_t;
    using ResourceHandle = uint64_t;

    class Resource : public Object {
    private:
        const ResourceHandle mHandle;

    public:
        Resource(PiperContext& context, const ResourceHandle handle) : Object(context), mHandle(handle) {}
        ResourceHandle getHandle() const noexcept {
            return mHandle;
        }
        virtual ~Resource() = default;
    };

    // not thread-safe
    class ResourceBinding : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ResourceBinding, Object);
        virtual ~ResourceBinding() = default;
        virtual void addInput(const SharedPtr<Resource>& resource) = 0;
        virtual void addOutput(const SharedPtr<Resource>& resource) = 0;
    };

    // TODO:type check in edge
    // TODO:stateless
    // TODO:rename:Payload
    class Payload : public Object {
    private:
        friend class Accelerator;
        virtual void append(const void* data, size_t size, const size_t alignment) = 0;

        template <typename T, typename = std::enable_if_t<std::is_trivial_v<T>>>
        void append(const T& data) {
            append(&data, sizeof(T), alignof(T));
        }

        virtual void addExtraInput(const SharedPtr<Resource>& resource) = 0;
        virtual void addExtraOutput(const SharedPtr<Resource>& resource) = 0;

    public:
        PIPER_INTERFACE_CONSTRUCT(Payload, Object);
        virtual ~Payload() = default;
    };

    class DataHolder {
    private:
        SharedPtr<void> mHolder;
        void* mPtr;

    public:
        template <typename T>
        DataHolder(SharedPtr<T> holder, void* ptr) : mHolder(std::move(holder)), mPtr(ptr) {}

        void* get() const noexcept {
            return mPtr;
        }
    };

    class Buffer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Buffer, Object);
        virtual ~Buffer() = default;
        virtual size_t size() const noexcept = 0;
        virtual void upload(Future<DataHolder> data) = 0;
        // TODO:provide destination
        virtual Future<DynamicArray<std::byte>> download() const = 0;
        virtual void reset() = 0;
        // TODO:immutable access limitation?
        virtual SharedPtr<Resource> ref() const = 0;
    };

    struct ExtraInputResource {
        const SharedPtr<Resource>& res;
    };

    struct ExtraOutputResource {
        const SharedPtr<Resource>& res;
    };

    struct ExtraInputOutputResource {
        const SharedPtr<Resource>& res;
    };

    struct InputResource final : public ExtraInputResource {};

    struct OutputResource final : public ExtraOutputResource {};

    struct InputOutputResource final : public ExtraInputOutputResource {};

    // TODO:share resource between Accelerators(CPU/GPU)
    // TODO:Allocator support
    // TODO:compiled kernel cache
    class Accelerator : public Object {
    private:
        virtual SharedPtr<Payload> createPayloadImpl() const = 0;

        void append(const SharedPtr<Payload>&) const {}

        template <typename First, typename... Args>
        auto append(const SharedPtr<Payload>& payload, const First& first, const Args&... args) const
            -> std::enable_if_t<std::is_base_of_v<ExtraInputResource, First>> {
            payload->addExtraInput(first.res);
            if constexpr(std::is_same_v<InputResource, First>)
                payload->append(first.res->getHandle());
            append(payload, args...);
        }

        template <typename First, typename... Args>
        auto append(const SharedPtr<Payload>& payload, const First& first, const Args&... args) const
            -> std::enable_if_t<std::is_base_of_v<ExtraOutputResource, First>> {
            payload->addExtraOutput(first.res);
            if constexpr(std::is_same_v<OutputResource, First>)
                payload->append(first.res->getHandle());
            append(payload, args...);
        }

        template <typename First, typename... Args>
        auto append(const SharedPtr<Payload>& payload, const First& first, const Args&... args) const
            -> std::enable_if_t<std::is_base_of_v<ExtraInputOutputResource, First>> {
            payload->addExtraInput(first.res);
            payload->addExtraOutput(first.res);
            if constexpr(std::is_same_v<InputOutputResource, First>)
                payload->append(first.res->getHandle());
            append(payload, args...);
        }

        template <typename First, typename... Args>
        auto append(const SharedPtr<Payload>& payload, const First& first, const Args&... args) const
            -> std::enable_if_t<std::is_trivial_v<First>> {
            payload->append(first);
            append(payload, args...);
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, Object);
        virtual ~Accelerator() = default;

        virtual Span<const CString> getSupportedLinkableFormat() const = 0;
        virtual SharedPtr<ResourceBinding> createResourceBinding() const = 0;

        template <typename... Args>
        SharedPtr<Payload> createPayload(const Args&... args) const {
            auto res = createPayloadImpl();
            append(res, args...);
            return res;
        }

        // TODO:Resource Name
        virtual SharedPtr<Resource> createResource(ResourceHandle handle) const = 0;
        virtual Future<SharedPtr<RunnableProgram>> compileKernel(const Span<Future<LinkableProgram>>& linkable,
                                                                 const String& entry) = 0;
        virtual Future<void> runKernel(uint32_t n, const Future<SharedPtr<RunnableProgram>>& kernel,
                                       const SharedPtr<Payload>& args) = 0;
        virtual void apply(Function<void, Context, CommandQueue> func, const SharedPtr<ResourceBinding>& binding) = 0;
        virtual Future<void> available(const SharedPtr<Resource>& resource) = 0;

        // TODO:page lock memory for CUDA
        // TODO:reduce copy for CPU or DMA
        virtual SharedPtr<Buffer> createBuffer(size_t size, size_t alignment) = 0;
    };
}  // namespace Piper
