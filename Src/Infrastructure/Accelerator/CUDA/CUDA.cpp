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
#include "../../../STL/Pair.hpp"
#include "../../../STL/USet.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <new>
#include <random>
#include <shared_mutex>
#include <utility>
#pragma warning(push, 0)
// TODO:__ldg?
//#include <cub/cub.cuh>  //utils
#include "../../../STL/List.hpp"
#include <cuda.h>
//#include <nvml.h>
#pragma warning(pop)

// TODO: use NCCL and Magnum IO
// TODO: use Graph?
// TODO: use cuLaunchHostFunc? (no blocking!!!)
// TODO: use TensorCore
// TODO: use Zero-Copy?
// TODO: nvSci?
// TODO: use Cooperative Groups?
// TODO: use CUB
// TODO: set stack size

namespace Piper {
    class CUDAContext;
    class CUDAAccelerator;

    static void checkCUDAResult(PiperContext& context, const SourceLocation& loc, CUresult res) {
        if(res == CUDA_SUCCESS) {
            /*
            // TODO: block option
            CUcontext ctx = nullptr;
            cuCtxGetCurrent(&ctx);
            if(ctx)
                res = cuCtxSynchronize();
            if(res == CUDA_SUCCESS)
            */
            return;
        }
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

    struct EventDeleter final {
        CUDAContext* context;
        void operator()(CUevent event) const;
    };

    struct ModuleDeleter final {
        CUDAContext& context;
        void operator()(CUmodule mod) const;
    };

    class CUDAFuture final : public FutureImpl {
    private:
        Pair<CUDAContext*, CUstream> mExecute;
        mutable SharedPtr<FutureImpl> mCommitFuture;
        mutable UniquePtr<CUevent_st, EventDeleter> mEvent;

    public:
        CUDAFuture(PiperContext& context, CUDAContext* ctx, SharedPtr<FutureImpl> commitFuture)
            : FutureImpl{ context }, mExecute{ ctx, nullptr }, mCommitFuture{ std::move(commitFuture) } {}
        Pair<CUDAContext*, CUstream> getExecute() const noexcept {
            return mExecute;
        }
        void committed(const CUstream stream, UniquePtr<CUevent_st, EventDeleter> event) noexcept {
            mExecute.second = stream;
            mEvent = std::move(event);
        }

        SharedPtr<FutureImpl> getFuture() const noexcept {
            return mCommitFuture;
        }
        CUevent getEvent() const noexcept {
            return mEvent.get();
        }
        [[nodiscard]] bool ready() const noexcept override;
        [[nodiscard]] bool fastReady() const noexcept override {
            return mExecute.second && !mEvent;
        }
        void wait() const override;
        [[nodiscard]] const void* storage() const override {
            // mCommitFuture->wait();
            return mCommitFuture->storage();
        }
    };

    using RandomEngine = std::mt19937_64;

    class CUDAContext final : public Context {
    private:
        CUDAAccelerator& mAccelerator;
        CUdevice mDevice;
        CUcontext mCUDAContext;
        DynamicArray<CUstream> mStreams;
        // TODO: remove mutex
        std::recursive_mutex mContextMutex;
        RandomEngine mRandomEngine;
        List<CUevent> mRecycledEvents;

        template <typename Result, typename Callable>
        Future<Result> spawnImpl(Callable&& callable, DynamicArray<SharedPtr<FutureImpl>> dependencies) {
            DynamicArray<SharedPtr<CUDAFuture>> events{ context().getAllocator() };

            for(auto&& dep : dependencies) {
                if(const auto cf = eastl::dynamic_shared_pointer_cast<CUDAFuture>(dep)) {
                    const auto ctx = cf->getExecute().first;
                    if(ctx == this) {
                        events.push_back(cf);
                        dep = cf->getFuture();
                    }
                }
            }

            auto&& scheduler = context().getScheduler();
            auto impl = scheduler.newFutureImpl(
                std::is_void_v<Result> ? 0 : sizeof(std::conditional_t<std::is_void_v<Result>, int, Result>),
                std::is_void_v<Result> ?
                    Closure<void*>{ context(), [](void*) {} } :
                    Closure<void*>{ context(), [](void* ptr) { std::destroy_at(static_cast<Result*>(ptr)); } },
                false);
            auto cudaFuture = makeSharedObject<CUDAFuture>(context(), this, impl);

            scheduler.spawnImpl(Closure<>{ context(),
                                           [this, ptr = impl->storage(), cf = cudaFuture.get(), wait = std::move(events),
                                            callable = std::forward<Callable>(callable)] {
                                               // TODO: schedule strategy
                                               const auto stream = selectOne().first;

                                               auto guard = makeCurrent();
                                               for(auto&& event : wait) {
                                                   const auto handle = event->getEvent();
                                                   if(handle)
                                                       checkCUDAResult(context(), PIPER_SOURCE_LOCATION(),
                                                                       cuStreamWaitEvent(stream, handle, CU_EVENT_WAIT_DEFAULT));
                                               }
                                               callable(mCUDAContext, stream, const_cast<void*>(ptr));
                                               cf->committed(stream, acquireEvent(stream));
                                           } },
                                Span<SharedPtr<FutureImpl>>{ dependencies.data(), dependencies.size() }, impl);

            return Future<Result>{ cudaFuture };
        }

        template <typename Callable, typename Result>
        std::enable_if_t<!std::is_void_v<Result>, Future<Result>>
        spawnDispatch(Callable&& callable, DynamicArray<SharedPtr<FutureImpl>> dependencies) {
            return spawnImpl<Result>(
                [call = std::forward<Callable>(callable)](CUcontext ctx, CUstream stream, void* ptr) {
                    new(static_cast<Result*>(ptr)) Result(call(ctx, stream));
                },
                std::move(dependencies));
        }

        template <typename Callable, typename Result>
        std::enable_if_t<std::is_void_v<Result>, Future<void>> spawnDispatch(Callable&& callable,
                                                                             DynamicArray<SharedPtr<FutureImpl>> dependencies) {
            return spawnImpl<Result>(
                [call = std::forward<Callable>(callable)](CUcontext ctx, CUstream stream, void*) { call(ctx, stream); },
                std::move(dependencies));
        }

    public:
        [[nodiscard]] std::lock_guard<std::recursive_mutex> makeCurrent() override {
            CUcontext current;
            checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuCtxGetCurrent(&current));
            if(mCUDAContext != current)
                checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuCtxSetCurrent(mCUDAContext));
            return std::lock_guard<std::recursive_mutex>{ mContextMutex };
        }

        CUDAContext(PiperContext& context, CUDAAccelerator& accelerator, const int idx, uint32_t streams)
            : Context{ context }, mAccelerator{ accelerator }, mDevice{ 0 },
              mCUDAContext{ nullptr }, mStreams{ context.getAllocator() },
              mRandomEngine{ static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) },
              mRecycledEvents{ context.getAllocator() } {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGet(&mDevice, idx));

            {
                // TODO: support devices which don't support memory pool
                int memoryPool;
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                cuDeviceGetAttribute(&memoryPool, CU_DEVICE_ATTRIBUTE_MEMORY_POOLS_SUPPORTED, mDevice));
                if(!memoryPool)
                    context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            }

            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuCtxCreate(&mCUDAContext, 0, mDevice));
            if(streams == 0) {
                int asyncEngine;
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                cuDeviceGetAttribute(&asyncEngine, CU_DEVICE_ATTRIBUTE_ASYNC_ENGINE_COUNT, mDevice));
                streams = static_cast<uint32_t>(asyncEngine);
            }
            auto guard = makeCurrent();

            for(uint32_t i = 0; i < streams; ++i) {
                CUstream stream;
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuStreamCreate(&stream, 0));
                mStreams.push_back(stream);
            }
        }

        [[nodiscard]] Pair<CUstream, bool> selectOne() {
            auto guard = makeCurrent();
            for(auto stream : mStreams) {
                const auto status = cuStreamQuery(stream);
                if(status == CUDA_SUCCESS)
                    return makePair(stream, true);
                if(status != CUDA_ERROR_NOT_READY)
                    checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), status);
            }

            // TODO: dynamic streams
            // TODO: better selection
            const std::uniform_int_distribution<size_t> gen{ 0, mStreams.size() - 1 };
            return makePair(mStreams[gen(mRandomEngine)], false);
        }

        UniquePtr<CUevent_st, EventDeleter> acquireEvent(const CUstream stream) {
            auto guard = makeCurrent();
            CUevent event = nullptr;
            if(!mRecycledEvents.empty()) {
                event = mRecycledEvents.front();
                mRecycledEvents.pop_front();

                const auto status = cuEventQuery(event);
                if(status != CUDA_SUCCESS) {
                    if(status == CUDA_ERROR_NOT_READY) {
                        mRecycledEvents.push_back(event);
                        event = nullptr;
                    } else {
                        checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), status);
                    }
                }
            }

            if(!event)
                checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuEventCreate(&event, CU_EVENT_DISABLE_TIMING));

            checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuEventRecord(event, stream));
            return { event, EventDeleter{ this } };
        }

        void releaseEvent(const CUevent event) {
            std::lock_guard<std::recursive_mutex> guard{ mContextMutex };
            mRecycledEvents.push_back(event);
        }

        [[nodiscard]] ContextHandle getHandle() const noexcept override {
            return reinterpret_cast<ContextHandle>(mCUDAContext);
        }

        [[nodiscard]] CUcontext getContextHandle() const noexcept {
            return mCUDAContext;
        }

        CommandQueueHandle select() override {
            return reinterpret_cast<CommandQueueHandle>(selectOne().first);
        }

        [[nodiscard]] CUDAAccelerator& getAccelerator() const {
            return mAccelerator;
        }

        template <typename Callable>
        auto spawn(Callable&& callable, DynamicArray<SharedPtr<FutureImpl>> dependencies) {
            using Result = std::invoke_result_t<Callable, CUcontext, CUstream>;
            return spawnDispatch<Callable, Result>(std::forward<Callable>(callable), std::move(dependencies));
        }

        ~CUDAContext() override {
            {
                auto guard = makeCurrent();
                checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuCtxSynchronize());
                for(auto stream : mStreams) {
                    checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuStreamDestroy(stream));
                }
                for(auto event : mRecycledEvents) {
                    checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuEventDestroy(event));
                }
            }
            checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuCtxDestroy(mCUDAContext));
        }
    };

    class CUDAKernelInstance final : public ResourceInstance {
    private:
        Future<UniquePtr<CUmod_st, ModuleDeleter>> mModule;

    public:
        CUDAKernelInstance(PiperContext& context, Future<UniquePtr<CUmod_st, ModuleDeleter>> module)
            : ResourceInstance{ context }, mModule{ std::move(module) } {}
        SharedPtr<FutureImpl> getFuture() const noexcept override {
            return mModule.raw();
        }
        ResourceHandle getHandle() const noexcept override {
            return reinterpret_cast<ResourceHandle>(mModule.getSync().get());
        }
    };

    static UniquePtr<CUmod_st, ModuleDeleter> compileKernel(CUDAContext* ctx, const Binary& binary) {
        auto&& logger = ctx->context().getLogger();
        if(logger.allow(LogLevel::Debug))
            logger.record(LogLevel::Debug, StringView{ reinterpret_cast<CString>(binary.data()), binary.size() },
                          PIPER_SOURCE_LOCATION());

        auto guard = ctx->makeCurrent();
        auto&& context = ctx->context();

        CUlinkState linkJIT;
        char errorBuffer[8192], infoBuffer[8192];

        CUjit_option options[] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, CU_JIT_INFO_LOG_BUFFER,
                                   CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES, CU_JIT_LOG_VERBOSE };
        void* errorBufferPtr = errorBuffer;
        void* infoBufferPtr = infoBuffer;
        auto errorBufferSize = static_cast<ptrdiff_t>(std::size(errorBuffer));
        auto infoBufferSize = static_cast<ptrdiff_t>(std::size(infoBuffer));
        void* values[] = { errorBufferPtr, reinterpret_cast<void*>(errorBufferSize), infoBufferPtr,
                           reinterpret_cast<void*>(infoBufferSize), reinterpret_cast<void*>(static_cast<ptrdiff_t>(1)) };
        static_assert(std::size(options) == std::size(values));
        checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                        cuLinkCreate(static_cast<unsigned>(std::size(options)), options, values, &linkJIT));
        auto JITDeleter = gsl::finally([&] { checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuLinkDestroy(linkJIT)); });

#define GENERATE_BUILTIN_IMPL(PA, PB, PC) PA "X" PB "x" PC PA "Y" PB "y" PC PA "Z" PB "z" PC
#define GENERATE_BUILTIN(NAME, REG) \
    GENERATE_BUILTIN_IMPL(".visible .func (.reg .u32 %res) " NAME, "()\n{\n    mov.u32 %res, " REG ".", ";\n    ret;\n}\n")

        constexpr auto entry = R"(
.version 7.2
.target sm_50
.address_size 64
)"

            GENERATE_BUILTIN("blockIdx", "%ctaid") GENERATE_BUILTIN("blockDim", "%ntid") GENERATE_BUILTIN("gridDim", "%nctaid")
                GENERATE_BUILTIN("threadIdx", "%tid")

                    R"(
.extern .func realEntry
(
	.param .b64 param0
)
;

.visible .entry cudaEntry(
  .param .b64 context
)
{
	.reg .b64 rd0;
	ld.param.b64 rd0, [context];

	{
	.param .b64 param0;
	st.param.b64 	[param0+0], rd0;

	call.uni 
	realEntry, 
	(
	param0
	);
	}
	ret;
}
        )";

#undef GENERATE_BUILTIN
#undef GENERATE_BUILTIN_IMPL

        checkCUDAResult(
            context, PIPER_SOURCE_LOCATION(),
            cuLinkAddData(linkJIT, CU_JIT_INPUT_PTX, const_cast<char*>(entry), strlen(entry) + 1, "Entry", 0, nullptr, nullptr));
        checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                        cuLinkAddData(linkJIT, CU_JIT_INPUT_PTX, const_cast<std::byte*>(binary.data()), binary.size(), "Program",
                                      0, nullptr, nullptr));

        void* cubin = nullptr;
        size_t size = 0;
        checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuLinkComplete(linkJIT, &cubin, &size));

        if(errorBuffer[0] != '\0' && logger.allow(LogLevel::Error))
            logger.record(LogLevel::Error, errorBuffer, PIPER_SOURCE_LOCATION());

        if(infoBuffer[0] != '\0' && logger.allow(LogLevel::Info))
            logger.record(LogLevel::Info, infoBuffer, PIPER_SOURCE_LOCATION());

        // String ptx{ reinterpret_cast<CString>(binary.data()), binary.size(), ctx->context().getAllocator() };
        CUmodule mod;
        checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuModuleLoadData(&mod, cubin));

        return UniquePtr<CUmod_st, ModuleDeleter>{ mod, ModuleDeleter{ *ctx } };
    }

    // TODO: cache kernel using PTX Compiler APIs
    class CUDAKernel final : public Kernel, public eastl::enable_shared_from_this<CUDAKernel> {
    private:
        Future<Binary> mBinary;
        UMap<Context*, SharedPtr<ResourceInstance>> mInstances;
        std::shared_mutex mMutex;

    public:
        CUDAKernel(PiperContext& context, Future<Binary> binary)
            : Kernel{ context }, mBinary{ std::move(binary) }, mInstances{ context.getAllocator() } {}
        SharedPtr<ResourceInstance> requireInstance(Context* ctx) override {
            return safeRequireInstance(mMutex, mInstances, ctx, [this, ctx] {
                auto&& scheduler = context().getScheduler();
                auto cudaCtx = dynamic_cast<CUDAContext*>(ctx);
                return makeSharedObject<CUDAKernelInstance>(context(), scheduler.spawn(compileKernel, cudaCtx, mBinary));
            });
        }
    };

    void EventDeleter::operator()(const CUevent event) const {
        auto guard = context->makeCurrent();
        context->releaseEvent(event);
    }

    void ModuleDeleter::operator()(const CUmodule mod) const {
        auto guard = context.makeCurrent();
        checkCUDAResult(context.context(), PIPER_SOURCE_LOCATION(), cuModuleUnload(mod));
    }

    bool CUDAFuture::ready() const noexcept {
        if(!mCommitFuture->ready())
            return false;

        if(!mEvent)
            return true;
        auto guard = mExecute.first->makeCurrent();
        const auto status = cuEventQuery(mEvent.get());
        if(status != CUDA_SUCCESS) {
            if(status != CUDA_ERROR_NOT_READY)
                checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), status);
            return false;
        }
        mEvent.reset();
        return true;
    }

    void CUDAFuture::wait() const {
        mCommitFuture->wait();

        if(!mEvent)
            return;
        auto guard = mExecute.first->makeCurrent();
        checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuEventSynchronize(mEvent.get()));
        mEvent.reset();
    }

    struct PinnedMemoryDeleter final {
        CUDAContext& context;
        void operator()(void* ptr) const {
            auto guard = context.makeCurrent();
            checkCUDAResult(context.context(), PIPER_SOURCE_LOCATION(), cuMemFreeHost(ptr));
        }
    };

    using CUDAPinnedMemory = UniquePtr<void, PinnedMemoryDeleter>;

    class CUDABufferInstance final : public ResourceInstance {
    private:
        CUDAContext& mContext;
        Optional<Future<Pair<CUdeviceptr, CUdeviceptr>>> mPtr;  // deferred construction
        SharedPtr<FutureImpl> mFuture;

    public:
        CUDABufferInstance(PiperContext& context, CUDAContext& ctx, const size_t size, const size_t alignment,
                           Function<void, Ptr> prepare)
            : ResourceInstance{ context }, mContext{ ctx }, mPtr{ eastl::nullopt } {
            auto src = context.getScheduler().spawn([this, &ctx, &context, size, func = std::move(prepare)] {
                void* ptr;
                {
                    auto guard = ctx.makeCurrent();
                    // TODO: memory pool
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuMemAllocHost(&ptr, size));
                }
                CUDAPinnedMemory holder{ ptr, PinnedMemoryDeleter{ ctx } };
                func(reinterpret_cast<Ptr>(ptr));
                return holder;
            });

            mPtr = ctx.spawn(
                [&context, size, alignment](CUcontext, const CUstream stream) {
                    CUdeviceptr ptr;
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                    cuMemAllocAsync(&ptr, size + (alignment == 1 ? 0 : alignment), stream));
                    auto aligned = ptr;
                    alignTo(aligned, alignment);
                    return makePair(aligned, ptr);
                },
                {});

            mFuture = ctx.spawn(
                             [this, &context, ptr = mPtr->getUnsafe().first, size,
                              source = std::move(src)](CUcontext, const CUstream stream) {
                                 checkCUDAResult(
                                     context, PIPER_SOURCE_LOCATION(),
                                     cuMemcpyHtoDAsync(static_cast<CUdeviceptr>(ptr), source.getUnsafe().get(), size, stream));
                             },
                             { { mPtr->raw(), src.raw() }, context.getAllocator() })
                          .raw();
        }

        ~CUDABufferInstance() override {
            auto [ctx, stream] = dynamic_cast<CUDAFuture*>(mFuture.get())->getExecute();
            if(ctx != &mContext) {
                mFuture->wait();
                stream = mContext.selectOne().first;
            }
            auto guard = mContext.makeCurrent();
            // NOTICE: no future
            checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuMemFreeAsync(mPtr->getUnsafe().second, stream));
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            return mPtr->getUnsafe().first;
        }
        [[nodiscard]] SharedPtr<FutureImpl> getFuture() const noexcept override {
            return mFuture;
        }
    };

    class CUDABuffer final : public Resource {
    private:
        SharedPtr<CUDABufferInstance> mSharedBuffer;  // TODO: unique buffer?

    public:
        CUDABuffer(PiperContext& context, CUDAContext& ctx, const size_t size, const size_t alignment,
                   Function<void, Ptr>&& prepare)
            : Resource{ context }, mSharedBuffer{ makeSharedObject<CUDABufferInstance>(context, ctx, size, alignment,
                                                                                       std::move(prepare)) } {}
        SharedPtr<ResourceInstance> requireInstance(Context*) override {
            return mSharedBuffer;
        }
    };

    class CUDATiledBufferInstance final : public ResourceInstance {
    private:
        CUDAContext& mContext;
        Optional<Future<Pair<CUdeviceptr, CUdeviceptr>>> mPtr;
        size_t mSize;
        SharedPtr<FutureImpl> mFuture;

    public:
        CUDATiledBufferInstance(PiperContext& context, CUDAContext& ctx, const size_t size, const size_t alignment)
            : ResourceInstance{ context }, mContext{ ctx }, mPtr{ eastl::nullopt }, mSize{ size } {
            mPtr = ctx.spawn(
                [&context, size, alignment](CUcontext, const CUstream stream) {
                    CUdeviceptr ptr;
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                    cuMemAllocAsync(&ptr, size + (alignment == 1 ? 0 : alignment), stream));
                    auto aligned = ptr;
                    alignTo(aligned, alignment);

                    // reset
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuMemsetD8Async(aligned, 0, size, stream));
                    return makePair(aligned, ptr);
                },
                {});
            mFuture = mPtr->raw();
        }
        void setFuture(SharedPtr<FutureImpl> future) {
            mFuture = std::move(future);
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            return mPtr->getUnsafe().first;
        }
        [[nodiscard]] SharedPtr<FutureImpl> getFuture() const noexcept override {
            return mFuture;
        }

        [[nodiscard]] Future<Binary> download() const {
            return mContext.spawn(
                [this, ptr = getHandle()](CUcontext, const CUstream stream) {
                    Binary res{ mSize, context().getAllocator() };
                    checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuMemcpyDtoHAsync(res.data(), ptr, mSize, stream));
                    // NOTICE: no copy!!!
                    return res;
                },
                { { mFuture }, context().getAllocator() });
        }
    };

    class CUDATiledBuffer final : public TiledOutput {
    private:
        // TODO: lazy allocation
        SharedPtr<CUDATiledBufferInstance> mMainBuffer;

    public:
        CUDATiledBuffer(PiperContext& context, CUDAContext& ctx, const size_t size, const size_t alignment)
            : TiledOutput{ context }, mMainBuffer{ makeSharedObject<CUDATiledBufferInstance>(context, ctx, size, alignment) } {}
        SharedPtr<ResourceInstance> requireInstance(Context*) override {
            return mMainBuffer;
        }
        [[nodiscard]] Future<Binary> download() const override {
            return mMainBuffer->download();
        }
    };

    class ResourceLookUpTableInstance final : public ResourceInstance {
    private:
        DynamicArray<SharedPtr<ResourceInstance>> mResourceInstances;

    public:
        ResourceLookUpTableInstance(PiperContext& context, Context* ctx, const DynamicArray<SharedPtr<Resource>>& resources)
            : ResourceInstance{ context }, mResourceInstances{ context.getAllocator() } {
            for(auto& inst : resources) {
                mResourceInstances.push_back(inst->requireInstance(ctx));
            }
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            context().getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
            return 0;
        }
        [[nodiscard]] SharedPtr<FutureImpl> getFuture() const noexcept override {
            context().getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
            return nullptr;
        }
        void collect(DynamicArray<SharedPtr<FutureImpl>>& futures, DynamicArray<ResourceHandle>& handles,
                     DynamicArray<CUDATiledBufferInstance*>& outputs) {
            DynamicArray<Pair<size_t, ResourceLookUpTableInstance*>> children{ context().getAllocator() };
            for(auto&& inst : mResourceInstances) {
                if(auto lut = dynamic_cast<ResourceLookUpTableInstance*>(inst.get())) {
                    children.push_back(makePair(handles.size(), lut));
                    handles.push_back(0);
                } else {
                    futures.push_back(inst->getFuture());
                    handles.push_back(inst->getHandle());
                    if(auto output = dynamic_cast<CUDATiledBufferInstance*>(inst.get())) {
                        outputs.push_back(output);
                    }
                }
            }
            for(auto&& [idx, lut] : children) {
                handles[idx] = handles.size();
                lut->collect(futures, handles, outputs);
            }
        }
    };

    class ResourceLookUpTableImpl final : public ResourceLookUpTable {
    private:
        DynamicArray<SharedPtr<Resource>> mResources;
        std::shared_mutex mMutex;
        UMap<Context*, SharedPtr<ResourceInstance>> mInstances;

    public:
        ResourceLookUpTableImpl(PiperContext& context, DynamicArray<SharedPtr<Resource>> resources)
            : ResourceLookUpTable{ context }, mResources{ std::move(resources) }, mInstances{ context.getAllocator() } {}
        SharedPtr<ResourceInstance> requireInstance(Context* ctx) override {
            return safeRequireInstance(mMutex, mInstances, ctx, [this, ctx] {
                return makeSharedObject<ResourceLookUpTableInstance>(context(), ctx, mResources);
            });
        }
    };

    class CUDAAccelerator final : public Accelerator {
    private:
        DynamicArray<UniquePtr<CUDAContext>> mContexts;
        DynamicArray<Context*> mContextReference;
        RandomEngine mRandomEngine;
        Future<SharedPtr<PITU>> mLibDeviceBitcode, mKernel;

    public:
        explicit CUDAAccelerator(PiperContext& context, const String& path, const SharedPtr<Config>& config)
            : Accelerator{ context }, mContexts{ context.getAllocator() }, mContextReference{ context.getAllocator() },
              mRandomEngine{ static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) },
              mLibDeviceBitcode{ context.getPITUManager().loadPITU(path + "/libdevice.10.bc") }, mKernel{
                  context.getPITUManager().loadPITU(path + "/Kernel.bc")
              } {
            uint32_t streams = 0;
            auto&& attr = config->viewAsObject();
            const auto iter = attr.find(String{ "Steams", context.getAllocator() });
            if(iter != attr.cend())
                streams = static_cast<uint32_t>(iter->second->get<uintmax_t>());
            int count;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGetCount(&count));
            if(count == 0)
                context.getErrorHandler().raiseException("No CUDA-capable device.", PIPER_SOURCE_LOCATION());

            DynamicArray<CUdevice> devices{ static_cast<size_t>(count), context.getAllocator() };
            for(auto idx = 0; idx < count; ++idx) {
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGet(&devices[idx], idx));
            }

            auto& logger = context.getLogger();
            if(logger.allow(LogLevel::Info)) {
                logger.record(LogLevel::Info, "detected " + toString(context.getAllocator(), count) + " CUDA-capable device(s)",
                              PIPER_SOURCE_LOCATION());
                for(auto idx = 0; idx < count; ++idx) {
                    const auto device = devices[idx];
                    char name[1024];
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGetName(name, sizeof(name), device));
                    size_t totalMemory;
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceTotalMem(&totalMemory, device));
                    logger.record(LogLevel::Info,
                                  toString(context.getAllocator(), idx) + " " + name + "  " +
                                      toString(context.getAllocator(), totalMemory >> 30) + " GB",
                                  PIPER_SOURCE_LOCATION());
                }
            }

            for(auto idx = 0; idx < count; ++idx) {
                mContexts.push_back(makeUniquePtr<CUDAContext>(context.getAllocator(), context, *this, idx, streams));
                mContextReference.push_back(mContexts.back().get());
                for(auto i = 0; i < count; ++i) {
                    if(idx == i)
                        continue;
                    int access;
                    checkCUDAResult(
                        context, PIPER_SOURCE_LOCATION(),
                        cuDeviceGetP2PAttribute(&access, CU_DEVICE_P2P_ATTRIBUTE_ACCESS_SUPPORTED, devices[idx], devices[i]));

                    int supportAtomic;
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(),
                                    cuDeviceGetP2PAttribute(&supportAtomic, CU_DEVICE_P2P_ATTRIBUTE_NATIVE_ATOMIC_SUPPORTED,
                                                            devices[idx], devices[i]));
                    // TODO: better support for multi-GPU
                    if(!(access && supportAtomic))
                        context.getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
                }
            }

            for(auto&& ctxA : mContexts) {
                for(auto&& ctxB : mContexts) {
                    if(ctxA == ctxB)
                        continue;
                    auto guard = ctxA->makeCurrent();
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuCtxEnablePeerAccess(ctxB->getContextHandle(), 0));
                }
            }
        }
        [[nodiscard]] Span<const CString> getSupportedLinkableFormat() const noexcept override {
            static CString format[] = { "LLVM IR" };
            return Span<const CString>{ format };
        }
        [[nodiscard]] CString getNativePlatform() const noexcept override {
            return "NVIDIA CUDA";
        }
        [[nodiscard]] SharedPtr<Kernel> compileKernel(const Span<LinkableProgram>& linkable,
                                                      UMap<String, String> staticRedirectedSymbols,
                                                      DynamicArray<String> dynamicSymbols, String entryFunction) override {
            staticRedirectedSymbols.insert(makePair(String{ "realEntry", context().getAllocator() }, std::move(entryFunction)));

            DynamicArray<Future<SharedPtr<PITU>>> modules{ context().getAllocator() };
            modules.reserve(linkable.size() + 2);
            modules.push_back(mKernel);
            modules.push_back(mLibDeviceBitcode);
            USet<uint64_t> inserted{ context().getAllocator() };

            for(auto&& mod : linkable) {
                if(mod.format != "LLVM IR")
                    context().getErrorHandler().raiseException("Unrecognized format \"" + mod.format + "\".",
                                                               PIPER_SOURCE_LOCATION());
                if(inserted.insert(mod.UID).second) {
                    // TODO: move
                    modules.push_back(eastl::get<Future<SharedPtr<PITU>>>(mod.exchange));
                }
            }

            auto linked =
                context().getPITUManager().linkPITU(modules, std::move(staticRedirectedSymbols), std::move(dynamicSymbols));
            // TODO: concurrency
            auto ptx = linked.getSync()->generateLinkable({ { "nvptx64" } });

            return makeSharedObject<CUDAKernel>(context(), eastl::get<Future<Binary>>(ptx.exchange));
        }

        [[nodiscard]] Future<void> launchKernelImpl(const Dim3& grid, const Dim3& block, SharedPtr<Kernel> kernel,
                                                    SharedPtr<ResourceLookUpTable> root, ArgumentPackage args) override {

            // TODO: launch on multi-GPU
            // TODO: tiled launching

            auto ctx = selectOne().first;

            auto entry = kernel->requireInstance(ctx);

            DynamicArray<SharedPtr<FutureImpl>> futures{ { entry->getFuture() }, context().getAllocator() };
            DynamicArray<ResourceHandle> handles{ context().getAllocator() };
            DynamicArray<CUDATiledBufferInstance*> outputs{ context().getAllocator() };

            eastl::dynamic_shared_pointer_cast<ResourceLookUpTableInstance>(root->requireInstance(ctx))
                ->collect(futures, handles, outputs);

            const auto bufferSize = args.data.size() + args.offset.size() * sizeof(decltype(args.offset)::value_type) +
                sizeof(uint32_t) * 2 + handles.size() * sizeof(ResourceHandle);
            auto launchBuffer =
                createBuffer(bufferSize, 128, [params = std::move(args), handleBuffer = std::move(handles)](Ptr ptr) {
                    // header
                    const auto handleBufferSize = static_cast<uint32_t>(handleBuffer.size() * sizeof(ResourceHandle));
                    *reinterpret_cast<uint32_t*>(ptr) = handleBufferSize + 2 * sizeof(uint32_t);
                    ptr += sizeof(uint32_t);
                    const auto offsetBufferSize =
                        static_cast<uint32_t>(params.offset.size() * sizeof(decltype(params.offset)::value_type));
                    *reinterpret_cast<uint32_t*>(ptr) = handleBufferSize + offsetBufferSize + 2 * sizeof(uint32_t);
                    ptr += sizeof(uint32_t);
                    // resource handles
                    memcpy(reinterpret_cast<void*>(ptr), handleBuffer.data(), handleBuffer.size() * sizeof(ResourceHandle));
                    ptr += handleBufferSize;
                    // argument params
                    memcpy(reinterpret_cast<void*>(ptr), params.offset.data(), offsetBufferSize);
                    ptr += offsetBufferSize;
                    memcpy(reinterpret_cast<void*>(ptr), params.data.data(), params.data.size());
                });

            auto launchBufferInstance = launchBuffer->requireInstance(ctx);
            futures.push_back(launchBufferInstance->getFuture());

            auto future =
                ctx->spawn(
                       [this, grid, block, func = std::move(entry), ref = std::move(root),
                        launchBufferRef = std::move(launchBuffer), kernelRef = std::move(kernel),
                        launchContextHandle = launchBufferInstance->getHandle()](CUcontext, const CUstream stream) {
                           auto launchParamBufferData = launchContextHandle;
                           void* kernelParams[] = { &launchParamBufferData };
                           const auto module = reinterpret_cast<CUmodule>(func->getHandle());
                           CUfunction cudaFunction;
                           checkCUDAResult(context(), PIPER_SOURCE_LOCATION(),
                                           cuModuleGetFunction(&cudaFunction, module, "cudaEntry"));
                           checkCUDAResult(context(), PIPER_SOURCE_LOCATION(),
                                           cuFuncSetAttribute(cudaFunction, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, 0));
                           checkCUDAResult(context(), PIPER_SOURCE_LOCATION(),
                                           cuFuncSetCacheConfig(cudaFunction, CU_FUNC_CACHE_PREFER_L1));
                           // TODO: set stack size

                           // TODO: better implementation
                           {
                               CUdevice device;
                               checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuCtxGetDevice(&device));
                               int limit;
                               checkCUDAResult(context(), PIPER_SOURCE_LOCATION(),
                                               cuDeviceGetAttribute(&limit, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, device));
                               if(static_cast<uint32_t>(limit) < block.x * block.y * block.z)
                                   context().getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
                           }

                           checkCUDAResult(context(), PIPER_SOURCE_LOCATION(),
                                           cuLaunchKernel(cudaFunction, grid.x, grid.y, grid.z, block.x, block.y, block.z, 0,
                                                          stream, kernelParams, nullptr));
                       },
                       futures)
                    .raw();

            for(auto&& output : outputs)
                output->setFuture(future);

            return Future<void>{ std::move(future) };
        }

        [[nodiscard]] SharedPtr<Resource> createBuffer(const size_t size, const size_t alignment,
                                                       Function<void, Ptr> prepare) override {
            return makeSharedObject<CUDABuffer>(context(), *mContexts.front(), size, alignment, std::move(prepare));
        }
        SharedPtr<ResourceLookUpTable> createResourceLUT(DynamicArray<SharedPtr<Resource>> resources) override {
            return makeSharedObject<ResourceLookUpTableImpl>(context(), std::move(resources));
        }
        SharedPtr<TiledOutput> createTiledOutput(const size_t size, const size_t alignment) override {
            return makeSharedObject<CUDATiledBuffer>(context(), *mContexts.front(), size, alignment);
        }

        // TODO: schedule strategy
        [[nodiscard]] Pair<CUDAContext*, CUstream> selectOne() {
            DynamicArray<Pair<CUDAContext*, CUstream>> streams{ context().getAllocator() };
            for(auto&& ctx : mContexts) {
                auto [stream, idle] = ctx->selectOne();
                const auto info = makePair(ctx.get(), stream);
                if(idle)
                    return info;
                streams.push_back(info);
            }
            const std::uniform_int_distribution<size_t> gen{ 0, streams.size() - 1 };
            return streams[gen(mRandomEngine)];
        }

        [[nodiscard]] const DynamicArray<Context*>& enumerateContexts() const override {
            return mContextReference;
        }
    };

    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path) : Module(context), mPath(path, context.getAllocator()) {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuInit(0));  // NOTICE: the flags parameter must be 0

            auto printVersion = [&](const CString type, const int version) {
                if(context.getLogger().allow(LogLevel::Info))
                    context.getLogger().record(LogLevel::Info,
                                               type + toString(context.getAllocator(), version / 1000) + "." +
                                                   toString(context.getAllocator(), version % 1000 / 10) + "." +
                                                   toString(context.getAllocator(), version % 10),
                                               PIPER_SOURCE_LOCATION());
            };
            printVersion("CUDA SDK Version : ", CUDA_VERSION);
            int version;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDriverGetVersion(&version));

            printVersion("CUDA Runtime Version : ", version);
        }
        [[nodiscard]] Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                                            const Future<void>& module) override {
            if(classID == "Accelerator") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<CUDAAccelerator>(context(), mPath, config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
