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
    // TODO:Parameter or Argument?
    class Parameter : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Parameter, Object);
        virtual ~Parameter() = default;
        virtual void bindInput(uint32_t slot, const SharedObject<Resource>& resource) = 0;
        virtual void bindOutput(uint32_t slot, const SharedObject<Resource>& resource) = 0;
        virtual void bindFloat32(uint32_t slot, const float value) = 0;
        virtual void bindFloat64(uint32_t slot, const double value) = 0;
        virtual void bindInt(uint32_t slot, const intmax_t value, const uint32_t bits) = 0;
        virtual void bindUInt(uint32_t slot, const uintmax_t value, const uint32_t bits) = 0;
        virtual void bindStructure(uint32_t slot, const void* data, const size_t size) = 0;

        template <typename T>
        void bindStructure(uint32_t slot, const T& data) {
            bindStructure(slot, &data, sizeof(data));
        }

        virtual void addExtraInput(const SharedObject<Resource>& resource) = 0;
        virtual void addExtraOutput(const SharedObject<Resource>& resource) = 0;
    };

    // TODO:share resource between Accelerators(CPU/GPU)
    // TODO:Allocator support
    // TODO: compiled kernel cache
    class Accelerator : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, Object);
        virtual ~Accelerator() = default;
        virtual Span<const CString> getSupportedLinkableFormat() const = 0;
        virtual SharedObject<ResourceBinding> createResourceBinding() const = 0;
        virtual SharedObject<Parameter> createParameters() const = 0;
        // TODO:Resource Name
        virtual SharedObject<Resource> createResource(const ResourceHandle handle) const = 0;
        virtual Future<SharedObject<RunnableProgram>> compileKernel(const Vector<Future<Vector<std::byte>>>& linkable,
                                                                    const String& entry) = 0;
        virtual void runKernel(uint32_t n, const Future<SharedObject<RunnableProgram>>& kernel,
                               const SharedObject<Parameter>& params) = 0;
        virtual void apply(Function<void, Context, CommandQueue> func, const SharedObject<ResourceBinding>& binding) = 0;
        virtual Future<void> available(const SharedObject<Resource>& resource) = 0;
    };
}  // namespace Piper
