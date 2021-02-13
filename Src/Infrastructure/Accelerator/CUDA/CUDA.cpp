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
#include <shared_mutex>
#include <utility>
#pragma warning(push, 0)
//#include <cub/cub.cuh>  //utils
#include "../../../STL/Pair.hpp"

#include <cuda.h>
#include <random>
#pragma warning(pop)

// TODO: use NCCL and Magnum IO

namespace Piper {
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

    // TODO:recycle Event
    struct EventDeleter final {
        PiperContext* context;
        void operator()(const CUevent event) const {
            checkCUDAResult(*context, PIPER_SOURCE_LOCATION(), cuEventDestroy(event));
        }
    };

    struct ModuleDeleter final {
        PiperContext& context;
        void operator()(const CUmodule mod) const {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuModuleUnload(mod));
        }
    };

    class CUDAKernel final : Uncopyable {
    private:
        PiperContext& mContext;
        UniquePtr<CUmod_st, ModuleDeleter> mModule;
        CUfunction mFunc;

    public:
        CUDAKernel(PiperContext& context, UniquePtr<CUmod_st, ModuleDeleter> mod, const CUfunction func)
            : mContext(context), mModule{ std::move(mod) }, mFunc{ func } {}
        void run(const int gridX, const int gridY, const int gridZ, const int blockX, const int blockY, const int blockZ,
                 const CUstream stream, const Binary& launchData) const {
            auto bufferSize = launchData.size();
            void* launchConfig[] = { CU_LAUNCH_PARAM_BUFFER_POINTER, const_cast<std::byte*>(launchData.data()),
                                     CU_LAUNCH_PARAM_BUFFER_SIZE, &bufferSize, CU_LAUNCH_PARAM_END };
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(),
                            cuLaunchKernel(mFunc, gridX, gridY, gridZ, blockX, blockY, blockZ, 0, stream, nullptr, launchConfig));
        }
    };

    class CUDAContext;
    class CUDAAccelerator;

    class CUDAFuture final : public FutureImpl {
    private:
        CUDAAccelerator& mAccelerator;
        mutable DynamicArray<Pair<CUDAContext*, UniquePtr<CUevent_st, EventDeleter>>> mEvents;

        void commit() {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }

    public:
        CUDAFuture(PiperContext& context, CUDAAccelerator& accelerator)
            : FutureImpl{ context }, mAccelerator{ accelerator }, mEvents{ context.getAllocator() } {}
        [[nodiscard]] bool ready() const noexcept override;
        [[nodiscard]] bool fastReady() const noexcept override {
            return false;
        }
        void wait() const override;
        [[nodiscard]] const void* storage() const override {
            return nullptr;
        }
    };
    // TODO:Launch on multi-GPU
    // TODO:LaunchKernel
    // TODO:Apply
    // TODO:Memcpy
    // TODO:Memset
    // TODO:MemOp

    struct CUDARunnableProgram final : RunnableProgramUntyped {
        uintmax_t UID;
        DynamicArray<Binary> binaries;
        String entry;

        CUDARunnableProgram(PiperContext& context, const uintmax_t UID, DynamicArray<Binary> binaries, String entry)
            : RunnableProgramUntyped(context), UID(UID), binaries{ std::move(binaries) }, entry{ std::move(entry) } {}
        void* lookup(const String&) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return nullptr;
        }
    };

    using RandomEngine = std::mt19937_64;

    class CUDAContext final : Unmovable {
    private:
        PiperContext& mContext;
        CUdevice mDevice;
        CUcontext mCUDAContext;
        DynamicArray<CUstream> mStreams;
        // TODO:remove mutex
        std::mutex mContextMutex;
        UMap<uintmax_t, CUDAKernel> mCompiledKernels;
        RandomEngine mRandomEngine;

    public:
        [[nodiscard]] std::lock_guard<std::mutex> makeCurrent() {
            CUcontext current;
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuCtxGetCurrent(&current));
            if(mCUDAContext != current)
                checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuCtxSetCurrent(mCUDAContext));
            return std::lock_guard<std::mutex>{ mContextMutex };
        }

        CUDAContext(PiperContext& context, const int idx, uint32_t streams)
            : mContext{ context }, mDevice{ 0 }, mCUDAContext{ nullptr }, mStreams{ context.getAllocator() },
              mCompiledKernels{ context.getAllocator() }, mRandomEngine{
                  static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
              } {
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGet(&mDevice, idx));

            {
                // TODO:support devices which don't support memory pool
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

            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuCtxEnablePeerAccess(mCUDAContext, 0));

            for(uint32_t i = 0; i < streams; ++i) {
                CUstream stream;
                checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuStreamCreate(&stream, 0));
                mStreams.push_back(stream);
            }
        }

        CUDAKernel& compile(const SharedPtr<CUDARunnableProgram>& program) {
            {
                const auto iter = mCompiledKernels.find(program->UID);
                if(iter != mCompiledKernels.cend())
                    return iter->second;
            }

            auto guard = makeCurrent();
            // TODO:cache
            CUlinkState linkJIT;
            char logBuffer[1024];

            CUjit_option options[] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, CU_JIT_CACHE_MODE };
            void* logBufferPtr = logBuffer;
            auto logBufferSize = static_cast<unsigned>(std::size(logBuffer));
            auto cacheMode = CU_JIT_CACHE_OPTION_CA;
            void* values[] = { &logBufferPtr, &logBufferSize, &cacheMode };
            static_assert(std::size(options) == std::size(values));
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(),
                            cuLinkCreate(static_cast<unsigned>(std::size(options)), options, values, &linkJIT));

            for(auto&& bin : program->binaries)
                checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(),
                                cuLinkAddData(linkJIT, CU_JIT_INPUT_PTX, bin.data(), bin.size(),
                                              "Unnamed" /* TODO:name of linkable*/, 0, nullptr, nullptr));

            void* cubin = nullptr;
            size_t size = 0;
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuLinkComplete(linkJIT, &cubin, &size));

            CUmodule mod;
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuModuleLoadData(&mod, cubin));
            UniquePtr<CUmod_st, ModuleDeleter> module{ mod, ModuleDeleter{ mContext } };

            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuLinkDestroy(linkJIT));

            CUfunction func;
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuModuleGetFunction(&func, mod, program->entry.c_str()));
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(),
                            cuFuncSetAttribute(func, CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES, 0));
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuFuncSetCacheConfig(func, CU_FUNC_CACHE_PREFER_L1));

            return mCompiledKernels.insert(makePair(program->UID, CUDAKernel{ mContext, std::move(module), func })).first->second;
        }

        [[nodiscard]] Pair<CUstream, bool> selectOne() {
            auto guard = makeCurrent();
            for(auto stream : mStreams) {
                const auto status = cuStreamQuery(stream);
                if(status == CUDA_SUCCESS)
                    return makePair(stream, true);
                if(status != CUDA_ERROR_NOT_READY)
                    checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), status);
            }
            // TODO:better selection
            const std::uniform_int_distribution<size_t> gen(0, mStreams.size() - 1);
            return makePair(mStreams[gen(mRandomEngine)], false);
        }

        ~CUDAContext() {
            {
                auto guard = makeCurrent();
                for(auto stream : mStreams)
                    checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuStreamDestroy(stream));
            }
            checkCUDAResult(mContext, PIPER_SOURCE_LOCATION(), cuCtxDestroy(mCUDAContext));
        }
    };

    bool CUDAFuture::ready() const noexcept {
        const_cast<CUDAFuture*>(this)->commit();
        auto ready = true;
        const auto end = std::remove_if(mEvents.begin(), mEvents.end(), [&, this](auto&& info) {
            auto&& [ctx, event] = info;
            auto guard = ctx->makeCurrent();
            const auto status = cuEventQuery(event.get());
            if(status != CUDA_SUCCESS) {
                if(status != CUDA_ERROR_NOT_READY)
                    checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), status);
                ready = false;
                return false;
            }
            return true;
        });
        mEvents.erase(end, mEvents.end());
        return ready;
    }

    void CUDAFuture::wait() const {
        const_cast<CUDAFuture*>(this)->commit();
        for(auto&& [ctx, event] : mEvents) {
            auto guard = ctx->makeCurrent();
            checkCUDAResult(context(), PIPER_SOURCE_LOCATION(), cuEventSynchronize(event.get()));
        }
    }

    // TODO:sub-buffer in multi-GPU for better I/O performance
    class CUDABuffer final : public Buffer {
    private:
        size_t mSize;
        size_t mAlignment;

    public:
        CUDABuffer(PiperContext& context, const size_t size, const size_t alignment)
            : Buffer{ context }, mSize{ size }, mAlignment{ alignment } {}
        [[nodiscard]] size_t size() const noexcept override {
            return mSize;
        }
        void upload(Function<void, Ptr> prepare) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }
        [[nodiscard]] Future<Binary> download() const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }
        void reset() override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }
        [[nodiscard]] ResourceShareMode getShareMode() const noexcept override {
            return ResourceShareMode::Sharable;
        }
        [[nodiscard]] Instance instantiateReference(const ContextHandle ctx) const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }
        [[nodiscard]] Instance instantiateMain() const override {
            return instantiateReference(nullptr);
        }
    };

    class CUDAAccelerator final : public Accelerator {
    private:
        DynamicArray<UniquePtr<CUDAContext>> mContexts;
        uintmax_t mModuleCount;
        RandomEngine mRandomEngine;

    public:
        explicit CUDAAccelerator(PiperContext& context, const SharedPtr<Config>& config)
            : Accelerator{ context }, mContexts{ context.getAllocator() }, mModuleCount{ 0 }, mRandomEngine{
                  static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
              } {
            const auto streams = static_cast<uint32_t>(config->at("Streams")->get<uintmax_t>());
            int count;
            checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceGetCount(&count));
            if(count == 0)
                context.getErrorHandler().raiseException("No CUDA-capable device.", PIPER_SOURCE_LOCATION());
            for(auto idx = 0; idx < count; ++idx) {
                mContexts.push_back(makeUniquePtr<CUDAContext>(context.getAllocator(), context, idx, streams));
                for(auto i = 0; i < count; ++i) {
                    if(idx == i)
                        continue;
                    int access;
                    checkCUDAResult(context, PIPER_SOURCE_LOCATION(), cuDeviceCanAccessPeer(&access, idx, i));
                    // TODO:better support for multi-GPU
                    if(!access)
                        context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
                }
            }
        }
        [[nodiscard]] Span<const CString> getSupportedLinkableFormat() const noexcept override {
            static CString format[] = { "NVPTX" };
            return Span<const CString>{ format };
        }
        [[nodiscard]] CString getNativePlatform() const noexcept override {
            return "NVIDIA CUDA";
        }
        [[nodiscard]] Future<SharedPtr<RunnableProgramUntyped>> compileKernelImpl(const Span<LinkableProgram>& linkable,
                                                                                  const String& entry) override {
            DynamicArray<Future<Binary>> binaries{ context().getAllocator() };
            USet<uint64_t> inserted{ context().getAllocator() };

            for(auto&& mod : linkable) {
                if(mod.format != "NVPTX")
                    context().getErrorHandler().raiseException("Unrecognized format \"" + mod.format + "\".",
                                                               PIPER_SOURCE_LOCATION());
                if(inserted.insert(mod.UID).second) {
                    // TODO:move
                    binaries.push_back(mod.exchange);
                }
            }

            // deferred compiling
            return context().getScheduler().spawn(
                [entry, UID = ++mModuleCount, ctx = &context()](DynamicArray<Binary> mods) {
                    return eastl::static_shared_pointer_cast<RunnableProgramUntyped>(
                        makeSharedObject<CUDARunnableProgram>(*ctx, UID, std::move(mods), entry));
                },
                context().getScheduler().wrap(binaries));
        }
        [[nodiscard]] Future<void> launchKernelImpl(const Dim3& grid, const Dim3& block,
                                                    const Future<SharedPtr<RunnableProgramUntyped>>& kernel,
                                                    const DynamicArray<ResourceView>& resources,
                                                    const ArgumentPackage& args) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return Future<void>{ nullptr };
        }

        [[nodiscard]] SharedPtr<Buffer> createBuffer(const size_t size, const size_t alignment) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return nullptr;
        }

        [[nodiscard]] Pair<CUDAContext*, CUstream> selectOne() {
            DynamicArray<Pair<CUDAContext*, CUstream>> streams{ context().getAllocator() };
            for(auto&& ctx : mContexts) {
                auto [stream, idle] = ctx->selectOne();
                const auto info = makePair(ctx.get(), stream);
                if(idle)
                    return info;
                streams.push_back(info);
            }
            // TODO:better selection
            const std::uniform_int_distribution<size_t> gen{ 0, streams.size() - 1 };
            return streams[gen(mRandomEngine)];
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
        [[nodiscard]] Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
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
