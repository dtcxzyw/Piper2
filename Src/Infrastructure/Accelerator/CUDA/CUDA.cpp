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

#define PIPER_EXPORT
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/Concurrency.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include "../../../STL/USet.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <new>
#include <utility>
#pragma warning(push, 0)
//#include <cub/cub.cuh>  //utils
#include <cuda.h>
#pragma warning(pop)

namespace Piper {
    // TODO: CUDA Task Graph

    static void checkCUDAResult(PiperContext& context, const SourceLocation& loc, const CUresult res) {
        if(res == CUDA_SUCCESS)
            return;
        auto name = "UNKNOWN", str = "Unknown error";
        cuGetErrorName(res, &name);
        cuGetErrorString(res, &str);
        context.getErrorHandler().raiseException(String{ "CUDA Error[", context.getAllocator() } + name + "] " + str + ".", loc);
    }

    struct ContextDeleter final {
        PiperContext& context;
        void operator()(const CUcontext ctx) const {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuCtxDestroy(ctx));
        }
    };

    struct StreamDeleter final {
        PiperContext& context;
        void operator()(const CUstream stream) const {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuStreamDestroy(stream));
        }
    };

    struct EventDeleter final {
        PiperContext& context;
        void operator()(const CUevent event) const {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuEventDestroy(event));
        }
    };

    struct ModuleDeleter final {
        PiperContext& context;
        void operator()(const CUmodule mod) const {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuModuleUnload(mod));
        }
    };

    class CUDAKernel final : public RunnableProgram {
    private:
        UniquePtr<CUmod_st, ModuleDeleter> mModule;
        CUfunction mFunc;

    public:
        CUDAKernel(PiperContext& context, UniquePtr<CUmod_st, ModuleDeleter> mod, const CUfunction func)
            : RunnableProgram{ context }, mModule{ std::move(mod) }, mFunc{ func } {}
        void* lookup(const String& symbol) override {
            // TODO:better interface
            return nullptr;
        }
    };

    class ResourceTracerImpl final : public ResourceTracer {
    private:
        SharedPtr<FutureImpl> mFuture;

    public:
        ResourceTracerImpl(PiperContext& context, const ResourceHandle handle) : ResourceTracer(context, handle) {}
        [[nodiscard]] SharedPtr<FutureImpl> getFuture() const {
            return mFuture;
        }
        void setFuture(SharedPtr<FutureImpl> future) {
            if(mFuture.unique() && !mFuture->ready())
                context().getErrorHandler().raiseException("Not handled future", PIPER_SOURCE_LOCATION());
            mFuture = std::move(future);
        }
    };

    class ResourceBindingImpl final : public ResourceBinding {
    private:
        DynamicArray<SharedPtr<Resource>> mInput;
        DynamicArray<SharedPtr<Resource>> mOutput;

    public:
        explicit ResourceBindingImpl(PiperContext& context)
            : ResourceBinding(context), mInput(context.getAllocator()), mOutput(context.getAllocator()) {}
        void addInput(const SharedPtr<Resource>& resource) override {
            mInput.push_back(resource);
        }
        void addOutput(const SharedPtr<Resource>& resource) override {
            mOutput.push_back(resource);
        }
        [[nodiscard]] DynamicArray<SharedPtr<FutureImpl>> getInput() const {
            DynamicArray<SharedPtr<FutureImpl>> input{ context().getAllocator() };
            input.reserve(mInput.size());
            for(auto&& in : mInput)
                input.push_back(dynamic_cast<ResourceTracerImpl&>(in->ref()).getFuture());
            return input;
        }
        void makeDirty(const SharedPtr<FutureImpl>& newFuture) {
            for(auto&& output : mOutput)
                dynamic_cast<ResourceTracerImpl&>(output->ref()).setFuture(newFuture);
        }
    };

    class PayloadImpl final : public Payload {
    private:
        DynamicArray<std::byte> mPayload;
        SharedPtr<ResourceBindingImpl> mResourceBinding;

    public:
        explicit PayloadImpl(PiperContext& context)
            : Payload(context), mPayload(STLAllocator{ context.getAllocator() }),
              mResourceBinding(makeSharedObject<ResourceBindingImpl>(context)) {}
        void append(const void* data, const size_t size, const size_t alignment) override {
            const auto rem = mPayload.size() % alignment;
            if(rem) {
                auto& logger = context().getLogger();
                if(logger.allow(LogLevel::Warning))
                    logger.record(LogLevel::Warning, "Inefficient payload layout", PIPER_SOURCE_LOCATION());
                mPayload.insert(mPayload.cend(), alignment - rem, std::byte{ 0 });
            }
            const auto* beg = static_cast<const std::byte*>(data);
            const auto* end = beg + size;
            mPayload.insert(mPayload.cend(), beg, end);
        }
        void addExtraInput(const SharedPtr<Resource>& resource) override {
            mResourceBinding->addInput(resource);
        }
        void addExtraOutput(const SharedPtr<Resource>& resource) override {
            mResourceBinding->addOutput(resource);
        }
        [[nodiscard]] DynamicArray<std::byte> data() const {
            return mPayload;
        }
        [[nodiscard]] SharedPtr<ResourceBindingImpl> getResourceBinding() const {
            return mResourceBinding;
        }
    };

    struct ContextScope final {
    private:
        PiperContext& mContext;
        SourceLocation mLocation;

    public:
        ContextScope(PiperContext& context, const SourceLocation& location, const CUcontext ctx)
            : mContext(context), mLocation(location) {
            checkCUDAResult(context, location, cuCtxPushCurrent(ctx));
        }
        ~ContextScope() {
            CUcontext prev;
            checkCUDAResult(mContext, mLocation, cuCtxPopCurrent(&prev));
        }
    };

    class CUDAAccelerator final : public Accelerator {
    private:
        std::thread mWorker;
        DynamicArray<UniquePtr<CUctx_st, ContextDeleter>> mContexts;

    public:
        explicit CUDAAccelerator(PiperContext& context, const SharedPtr<Config>& config)
            : Accelerator{ context }, mContexts{ context.getAllocator() } {
            const auto streams = static_cast<uint32_t>(config->at("Streams")->get<uintmax_t>());
            int count;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGetCount(&count));
            if(count == 0)
                context.getErrorHandler().raiseException("No CUDA-capable device.", PIPER_SOURCE_LOCATION());

            // TODO: multi GPU
            CUdevice primary;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGet(&primary, 0));

            CUcontext ctx;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuCtxCreate(&ctx, 0, primary));
            mContexts.push_back({ ctx, ContextDeleter{ context } });
        }
        [[nodiscard]] Span<const CString> getSupportedLinkableFormat() const override {
            static CString format[] = { "NVPTX" };
            return Span<const CString>{ format };
        }
        [[nodiscard]] SharedPtr<ResourceBinding> createResourceBinding() const override {
            return eastl::static_shared_pointer_cast<ResourceBinding>(makeSharedObject<ResourceBindingImpl>(context()));
        }
        [[nodiscard]] SharedPtr<Payload> createPayloadImpl() const override {
            return eastl::static_shared_pointer_cast<Payload>(makeSharedObject<PayloadImpl>(context()));
        }
        [[nodiscard]] UniqueObject<ResourceTracer> createResourceTracer(const ResourceHandle handle) const override {
            return makeUniqueObject<ResourceTracer, ResourceTracerImpl>(context(), handle);
        }
        Future<SharedPtr<RunnableProgram>> compileKernel(const Span<LinkableProgram>& linkable, const String& entry) override {
            return Future<SharedPtr<RunnableProgram>>{ nullptr };
        }
        Future<void> runKernel(const uint32_t n, const Future<SharedPtr<RunnableProgram>>& kernel,
                               const SharedPtr<Payload>& payload) override {
            return Future<void>{ nullptr };
        }
        void apply(Function<void, Context, CommandQueue> func, const SharedPtr<ResourceBinding>& binding) override {}
        Future<void> available(const SharedPtr<Resource>& resource) override {
            return Future<void>{ dynamic_cast<ResourceTracerImpl&>(resource->ref()).getFuture() };
        }
        SharedPtr<Buffer> createBuffer(const size_t size, const size_t alignment) override {
            return nullptr;
        }
    };

    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path) : Module(context), mPath(path, context.getAllocator()) {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuInit(0));  // the flags parameter must be 0

            int version;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDriverGetVersion(&version));
            if(context.getLogger().allow(LogLevel::Info))
                context.getLogger().record(LogLevel::Info,
                                           "CUDA Version " + toString(context.getAllocator(), version / 1000) + "." +
                                               toString(context.getAllocator(), version % 1000 / 10),
                                           PIPER_SOURCE_LOCATION());
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Accelerator") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<CUDAAccelerator>(context(), config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
