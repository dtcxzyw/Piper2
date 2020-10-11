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
#include "../../STL/Function.hpp"
#include "../../STL/String.hpp"
#include "../../STL/Vector.hpp"
#include "../Object.hpp"
#include "Allocator.hpp"
#include "Concurrency.hpp"

namespace Piper {

    class RunnableProgram;

    using CommandQueue = uint64_t;
    using Context = uint64_t;
    using ResourceHandle = uint64_t;

    class Resource : public Object {
    private:
        ResourceHandle mHandle;

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
        virtual void addInput(const SharedObject<Resource>& resource) = 0;
        virtual void addOutput(const SharedObject<Resource>& resource) = 0;
    };

    // TODO:type check in edge
    // TODO:stateless
    // TODO:rename:Payload
    class Argument : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Argument, Object);
        virtual ~Argument() = default;
        virtual void appendInput(const SharedObject<Resource>& resource) = 0;
        virtual void appendOutput(const SharedObject<Resource>& resource) = 0;
        virtual void appendInputOutput(const SharedObject<Resource>& resource) = 0;
        virtual void append(const void* data, size_t size, const size_t alignment) = 0;

        template <typename T, typename = std::enable_if_t<std::is_trivial_v<T>>>
        void append(const T& data) {
            append(&data, sizeof(T), alignof(T));
        }

        virtual void addExtraInput(const SharedObject<Resource>& resource) = 0;
        virtual void addExtraOutput(const SharedObject<Resource>& resource) = 0;
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
        virtual Future<Vector<std::byte>> download() const = 0;
        virtual void reset() = 0;
        // TODO:immutable access limitation?
        virtual SharedObject<Resource> ref() const = 0;
    };

    // TODO:share resource between Accelerators(CPU/GPU)
    // TODO:Allocator support
    // TODO:compiled kernel cache
    class Accelerator : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, Object);
        virtual ~Accelerator() = default;

        virtual Span<const CString> getSupportedLinkableFormat() const = 0;
        virtual SharedObject<ResourceBinding> createResourceBinding() const = 0;
        virtual SharedObject<Argument> createArgument() const = 0;
        // TODO:Resource Name
        virtual SharedObject<Resource> createResource(ResourceHandle handle) const = 0;
        virtual Future<SharedObject<RunnableProgram>> compileKernel(const Vector<Future<Vector<std::byte>>>& linkable,
                                                                    const String& entry) = 0;
        virtual void runKernel(uint32_t n, const Future<SharedObject<RunnableProgram>>& kernel,
                               const SharedObject<Argument>& args) = 0;
        virtual void apply(Function<void, Context, CommandQueue> func, const SharedObject<ResourceBinding>& binding) = 0;
        virtual Future<void> available(const SharedObject<Resource>& resource) = 0;

        // TODO:page lock memory for CUDA
        // TODO:reduce copy for CPU or DMA
        virtual SharedObject<Buffer> createBuffer(size_t size, size_t alignment) = 0;
    };
}  // namespace Piper
