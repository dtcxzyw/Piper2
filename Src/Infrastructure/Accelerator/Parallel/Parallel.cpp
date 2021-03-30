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
#include "Shared.hpp"
#include <new>
#include <utility>
#pragma warning(push, 0)
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
// use LLJIT
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/ManagedStatic.h>
//#define NOMINMAX
//#include <Windows.h>
#pragma warning(pop)

extern "C" void __chkstk();

namespace Piper {
    // TODO: set llvm::Context yield callback
    // TODO: better implementation for cross-platform
    /*
    static CommandQueueHandle currentThreadID() {
        return static_cast<CommandQueueHandle>(GetCurrentThreadId());
    }
    */

    static SharedPtr<ResourceInstance> dummy = nullptr;
    static SharedPtr<FutureImpl> dummyFuture = nullptr;

    template <typename T>
    auto getLLVMResult(PiperContext& context, const SourceLocation& loc, llvm::Expected<T> value) {
        if(value)
            return std::move(std::move(value).get());
        context.getErrorHandler().raiseException(("LLVM error: " + llvm::toString(value.takeError())).c_str(), loc);
    }

    static std::unique_ptr<llvm::Module> binary2Module(PiperContext& context, const Binary& binary, llvm::LLVMContext& ctx) {
        const auto beg = reinterpret_cast<const uint8_t*>(binary.data());
        const auto end = beg + binary.size();

        return getLLVMResult(context, PIPER_SOURCE_LOCATION(),
                             llvm::parseBitcodeFile(
                                 llvm::MemoryBufferRef{ toStringRef(llvm::ArrayRef<uint8_t>{ beg, end }), "llvm module" }, ctx));
    }

    class LLVMLoggerWrapper : public llvm::raw_ostream {
    private:
        Logger& mLogger;
        SourceLocation mLocation;
        DynamicArray<char> mData;

    public:
        explicit LLVMLoggerWrapper(PiperContext& context, const SourceLocation& location)
            : raw_ostream(true), mLogger(context.getLogger()), mLocation(location), mData(context.getAllocator()) {}
        [[nodiscard]] uint64_t current_pos() const override {
            return mData.size();
        }
        void write_impl(const char* ptr, const size_t size) override {
            mData.insert(mData.cend(), ptr, ptr + size);
        }
        void flushToLogger() {
            // ReSharper disable once CppRedundantQualifier
            llvm::raw_ostream::flush();
            if(!mData.empty() && mLogger.allow(LogLevel::Error)) {
                mLogger.record(LogLevel::Error, StringView{ mData.data(), mData.size() }, mLocation);
                mLogger.flush();
            }
        }
    };

    class LLVMKernel final : public Kernel, public eastl::enable_shared_from_this<LLVMKernel> {
    private:
        Future<std::unique_ptr<llvm::orc::LLJIT>> mJIT;
        String mEntry;

    public:
        LLVMKernel(PiperContext& context, Future<std::unique_ptr<llvm::orc::LLJIT>> JIT, String entry)
            : Kernel{ context }, mJIT{ std::move(JIT) }, mEntry{ std::move(entry) } {}
        const SharedPtr<ResourceInstance>& requireInstance(Context*) override {
            context().getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
            return dummy;
        }
        void* lookUpSync() const {
            return reinterpret_cast<void*>(
                getLLVMResult(context(), PIPER_SOURCE_LOCATION(), mJIT.getSync()->lookup(mEntry.c_str())).getAddress());
        }
        const SharedPtr<FutureImpl>& getFuture() const {
            return mJIT.raw();
        }
    };

    class BufferInstance final : public ResourceInstance {
    private:
        Allocator& mAllocator;
        Ptr mPtr;
        Future<void> mFuture;

    public:
        BufferInstance(PiperContext& context, const size_t size, const size_t alignment, Function<void, Ptr> prepare)
            : ResourceInstance{ context }, mAllocator{ context.getAllocator() }, mPtr{ mAllocator.alloc(size, alignment) },
              mFuture{ context.getScheduler().spawn([this, func = std::move(prepare)] { func(mPtr); }) } {}
        ~BufferInstance() override {
            mAllocator.free(mPtr);
        }
        const SharedPtr<FutureImpl>& getFuture() const noexcept override {
            return mFuture.raw();
        }
        ResourceHandle getHandle() const noexcept override {
            return static_cast<ResourceHandle>(mPtr);
        }
    };

    class Buffer final : public Resource {
    private:
        SharedPtr<ResourceInstance> mInstance;

    public:
        Buffer(PiperContext& context, const size_t size, const size_t alignment, Function<void, Ptr> prepare)
            : Resource{ context }, mInstance{ makeSharedObject<BufferInstance>(context, size, alignment, std::move(prepare)) } {}
        const SharedPtr<ResourceInstance>& requireInstance(Context*) override {
            return mInstance;
        }
    };

    class TiledBufferInstance final : public ResourceInstance {
    private:
        Allocator& mAllocator;
        Ptr mPtr;
        size_t mSize;
        SharedPtr<FutureImpl> mFuture;

    public:
        TiledBufferInstance(PiperContext& context, const size_t size, const size_t alignment)
            : ResourceInstance{ context },
              mAllocator{ context.getAllocator() }, mPtr{ mAllocator.alloc(size, alignment) }, mSize{ size }, mFuture{
                  context.getScheduler().spawn([this] { memset(reinterpret_cast<void*>(mPtr), 0, mSize); }).raw()
              } {}
        ~TiledBufferInstance() override {
            mAllocator.free(mPtr);
        }
        [[nodiscard]] const SharedPtr<FutureImpl>& getFuture() const noexcept override {
            return mFuture;
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            return mPtr;
        }
        void setFuture(SharedPtr<FutureImpl> future) {
            mFuture = std::move(future);
        }
        [[nodiscard]] Future<Binary> download() const {
            return context().getScheduler().spawn(
                [this](PlaceHolder) {
                    const auto beg = reinterpret_cast<std::byte*>(mPtr);
                    const auto end = beg + mSize;
                    Binary res{ beg, end, context().getAllocator() };
                    return res;
                },
                Future<void>(mFuture));
        }
    };

    class TiledBuffer final : public TiledOutput {
    private:
        SharedPtr<TiledBufferInstance> mInstance;
        SharedPtr<ResourceInstance> mRef;

    public:
        TiledBuffer(PiperContext& context, const size_t size, const size_t alignment)
            : TiledOutput{ context }, mInstance{ makeSharedObject<TiledBufferInstance>(context, size, alignment) }, mRef{
                  mInstance
              } {}
        const SharedPtr<ResourceInstance>& requireInstance(Context*) override {
            return mRef;
        }
        void markDirty(SharedPtr<FutureImpl> future) override {
            mInstance->setFuture(std::move(future));
        }
        [[nodiscard]] Future<Binary> download() const override {
            return mInstance->download();
        }
    };

    // TODO: become more general or move to PiperCore?
    static void parseNative(PiperContext& context, const Binary& data, UMap<String, void*>& symbol) {
        static_assert(sizeof(void*) == 8);
        const auto* ptr = reinterpret_cast<const char8_t*>(data.data());
        const auto* end = ptr + data.size();
        if(data.size() < 6 || std::string_view{ ptr, 6 } != "Native")
            context.getErrorHandler().raiseException("Bad header in native linkable file.", PIPER_SOURCE_LOCATION());
        ptr += 6;
        while(ptr < end) {
            const auto* beg = ptr;
            while(*ptr != '\0') {
                ++ptr;
                if(ptr >= end)
                    context.getErrorHandler().raiseException("The native linkable file is corrupted.", PIPER_SOURCE_LOCATION());
            }

            auto name = String{ beg, ptr, context.getAllocator() };
            if(ptr + 9 > end)
                context.getErrorHandler().raiseException("The native linkable file is corrupted.", PIPER_SOURCE_LOCATION());
            auto* func = reinterpret_cast<void*>(*reinterpret_cast<const uint64_t*>(ptr + 1));
            ptr += 9;
            symbol.insert({ std::move(name), func });
        }
    }

    struct TaskContextImplEx {
        TaskContextImpl ctx;
        const ArgumentPackage& args;
    };

    // TODO: move to kernel
    static void piperGetArgument(const TaskContext context, const uint32_t index, void* ptr) {
        auto&& ctxEx = *reinterpret_cast<const TaskContextImplEx*>(context);
        const auto [offset, size] = ctxEx.args.offset[index];
        memcpy(ptr, ctxEx.args.data.data() + offset, size);
    }

    class ResourceLookUpTableInstance final : public ResourceInstance {
    private:
        DynamicArray<SharedPtr<ResourceInstance>> mResourceInstances;

    public:
        ResourceLookUpTableInstance(PiperContext& context, const DynamicArray<SharedPtr<Resource>>& resources)
            : ResourceInstance{ context }, mResourceInstances{ context.getAllocator() } {
            for(auto& inst : resources) {
                mResourceInstances.push_back(inst->requireInstance(nullptr));
            }
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            context().getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
            return 0;
        }
        [[nodiscard]] const SharedPtr<FutureImpl>& getFuture() const noexcept override {
            context().getErrorHandler().notSupported(PIPER_SOURCE_LOCATION());
            return dummyFuture;
        }
        void collect(DynamicArray<SharedPtr<FutureImpl>>& futures, DynamicArray<ResourceHandle>& handles,
                     DynamicArray<size_t>& offsets) {
            DynamicArray<Pair<size_t, ResourceLookUpTableInstance*>> children{ context().getAllocator() };
            for(auto&& inst : mResourceInstances) {
                if(auto lut = dynamic_cast<ResourceLookUpTableInstance*>(inst.get())) {
                    children.push_back(makePair(handles.size(), lut));
                    offsets.push_back(handles.size());
                    handles.push_back(0);
                } else {
                    futures.push_back(inst->getFuture());
                    handles.push_back(inst->getHandle());
                }
            }
            for(auto&& [idx, lut] : children) {
                handles[idx] = handles.size();
                lut->collect(futures, handles, offsets);
            }
        }
    };

    class ResourceLookUpTableImpl final : public ResourceLookUpTable {
    private:
        SharedPtr<ResourceInstance> mInstance;

    public:
        ResourceLookUpTableImpl(PiperContext& context, const DynamicArray<SharedPtr<Resource>>& resources)
            : ResourceLookUpTable{ context }, mInstance{ makeSharedObject<ResourceLookUpTableInstance>(context, resources) } {}
        const SharedPtr<ResourceInstance>& requireInstance(Context* ctx) override {
            return mInstance;
        }
    };

    // TODO: dummy context
    class ParallelAccelerator final : public Accelerator {
    private:
        static constexpr uint32_t chunkSize = 1024;
        LinkableProgram mRuntime;
        // TODO: support context
        DynamicArray<Context*> mContexts;

    public:
        explicit ParallelAccelerator(PiperContext& context, LinkableProgram runtime)
            : Accelerator{ context }, mRuntime{ std::move(runtime) }, mContexts{ context.getAllocator() } {}
        [[nodiscard]] Span<const CString> getSupportedLinkableFormat() const noexcept override {
            // about LLVM IR Version: https://llvm.org/docs/DeveloperPolicy.html#ir-backwards-compatibility
            static CString format[] = { "LLVM IR", "Native" };
            return Span<const CString>{ format };
        }
        [[nodiscard]] CString getNativePlatform() const noexcept override {
            return "CPU";
        }
        [[nodiscard]] SharedPtr<Kernel> compileKernel(const Span<LinkableProgram>& linkable,
                                                      UMap<String, String> staticRedirectedSymbols,
                                                      DynamicArray<String> dynamicSymbols, String entryFunction) override {
            DynamicArray<Future<SharedPtr<PITU>>> modules{ context().getAllocator() };
            modules.reserve(linkable.size() + 1);
            USet<size_t> hashPool{ context().getAllocator() };
            UMap<String, void*> nativeSymbol{ context().getAllocator() };
            auto& scheduler = context().getScheduler();

            auto addModule = [&](LinkableProgram& unit) {
                if(!hashPool.insert(unit.UID).second)
                    return;

                if(unit.format == "LLVM IR") {
                    auto&& exchange = eastl::get<Future<SharedPtr<PITU>>>(unit.exchange);
                    modules.push_back(exchange);
                } else if(unit.format == "Native") {
                    // TODO: concurrency
                    parseNative(context(), eastl::get<Future<Binary>>(unit.exchange).getSync(), nativeSymbol);
                } else
                    context().getErrorHandler().raiseException("Unsupported linkable format " +
                                                                   String{ unit.format, context().getAllocator() },
                                                               PIPER_SOURCE_LOCATION());
            };
            addModule(mRuntime);
            for(auto&& unit : linkable)
                addModule(unit);

            auto linked =
                context().getPITUManager().linkPITU(modules, std::move(staticRedirectedSymbols), std::move(dynamicSymbols));

            auto engine = scheduler.spawn(
                [ctx = &context(), nativeSymbol, this](const SharedPtr<PITU>& linkedPITU) {
                    auto llCtx = std::make_unique<llvm::LLVMContext>();
                    auto stage = ctx->getErrorHandler().enterStage("link PITU", PIPER_SOURCE_LOCATION());

                    // TODO: concurrency?
                    auto linkable = linkedPITU->generateLinkable({ { "LLVM IR Bitcode" } });
                    auto mod = binary2Module(context(), eastl::get<Future<Binary>>(linkable.exchange).getSync(), *llCtx);

                    stage.next("build JIT", PIPER_SOURCE_LOCATION());

                    const auto triple = llvm::sys::getProcessTriple();
                    const auto cpu = llvm::sys::getHostCPUName();
                    llvm::StringMap<bool> features;
                    llvm::sys::getHostCPUFeatures(features);

                    auto& logger = ctx->getLogger();
                    llvm::orc::JITTargetMachineBuilder JTMB{ llvm::Triple{ triple } };
                    JTMB.setCPU(cpu.str());

                    for(auto&& feature : features)
                        if(feature.second)
                            JTMB.getFeatures().AddFeature(feature.first());

                    if(logger.allow(LogLevel::Info)) {
                        String featureInfo{ ctx->getAllocator() };
                        for(auto&& feature : features)
                            if(feature.second)
                                featureInfo = featureInfo + feature.first().data() + ",";
                        if(featureInfo.size())
                            featureInfo.pop_back();
                        logger.record(LogLevel::Info,
                                      String{ "Triple: ", ctx->getAllocator() } + triple.data() + "\nCPU: " + cpu.data() +
                                          "\nFeatures: " + featureInfo,
                                      PIPER_SOURCE_LOCATION());
                    }

                    // TODO: test settings
                    // TODO: FP Precise
                    JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);  //-O3
                    JTMB.setRelocationModel(llvm::Reloc::PIC_);
                    JTMB.setCodeModel(llvm::CodeModel::Small);

                    auto& options = JTMB.getOptions();
                    options.EmulatedTLS = false;
                    options.TLSSize = 0;

                    auto engine = getLLVMResult(
                        *ctx, PIPER_SOURCE_LOCATION(),
                        std::move(llvm::orc::LLJITBuilder{}     //.setNumCompileThreads(std::thread::hardware_concurrency())
                                      .setNumCompileThreads(0)  // NOTE: LLVM hasn't fix the bug in LLJIT construction.
                                      .setJITTargetMachineBuilder(std::move(JTMB)))
                            .create());

                    mod->setDataLayout(engine->getDataLayout());

                    if(auto err = engine->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(llCtx))))
                        ctx->getErrorHandler().raiseException(llvm::toString(std::move(err)).c_str(), PIPER_SOURCE_LOCATION());

                    {
                        const auto flag =
                            llvm::JITSymbolFlags::Callable | llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Absolute;
                        llvm::orc::SymbolMap map = {
#define PIPER_NATIVE_FUNC(name)                                                 \
    {                                                                           \
        engine->getExecutionSession().intern(#name), llvm::JITEvaluatedSymbol { \
            reinterpret_cast<llvm::JITTargetAddress>(name), flag                \
        }                                                                       \
    }
                            // common math functions
                            PIPER_NATIVE_FUNC(cosf), PIPER_NATIVE_FUNC(sinf), PIPER_NATIVE_FUNC(fabsf), PIPER_NATIVE_FUNC(fmaxf),
                            PIPER_NATIVE_FUNC(fminf), PIPER_NATIVE_FUNC(sqrtf), PIPER_NATIVE_FUNC(cbrtf),
                            PIPER_NATIVE_FUNC(hypotf), PIPER_NATIVE_FUNC(acosf), PIPER_NATIVE_FUNC(atan2f),
                            PIPER_NATIVE_FUNC(asinf), PIPER_NATIVE_FUNC(floorf), PIPER_NATIVE_FUNC(ceilf),
                            PIPER_NATIVE_FUNC(remainderf), PIPER_NATIVE_FUNC(logf), PIPER_NATIVE_FUNC(expf),
                            // TODO: cross C library
                            PIPER_NATIVE_FUNC(_fdtest),
                            // Memory operations
                            PIPER_NATIVE_FUNC(memset), PIPER_NATIVE_FUNC(memcpy), PIPER_NATIVE_FUNC(memmove),
                            // Runtime interface
                            PIPER_NATIVE_FUNC(piperGetArgument),
                            // MSVC internal functions???
                            PIPER_NATIVE_FUNC(__chkstk)
#undef PIPER_NATIVE_FUNC
                        };
                        for(const auto& symbol : nativeSymbol)
                            map.insert(std::make_pair(
                                engine->getExecutionSession().intern(llvm::StringRef{ symbol.first.data(), symbol.first.size() }),
                                llvm::JITEvaluatedSymbol{ reinterpret_cast<llvm::JITTargetAddress>(symbol.second), flag }));

                        if(auto err = engine->getMainJITDylib().define(
                               std::make_unique<llvm::orc::AbsoluteSymbolsMaterializationUnit>(map)))
                            ctx->getErrorHandler().raiseException(llvm::toString(std::move(err)).c_str(),
                                                                  PIPER_SOURCE_LOCATION());
                    }

                    return engine;
                },
                linked);
            return makeSharedObject<LLVMKernel>(context(), std::move(engine), std::move(entryFunction));
        }
        Future<void> launchKernelImpl(const Dim3& size, SharedPtr<Kernel> kernel, SharedPtr<ResourceLookUpTable> root,
                                      ArgumentPackage args) override {
            // TODO: for small n, run in this thread

            auto& scheduler = context().getScheduler();
            auto func = eastl::dynamic_shared_pointer_cast<LLVMKernel>(kernel);

            DynamicArray<SharedPtr<FutureImpl>> futures{ { func->getFuture() }, context().getAllocator() };
            DynamicArray<ResourceHandle> handles{ context().getAllocator() };
            DynamicArray<size_t> offsets{ context().getAllocator() };

            eastl::dynamic_shared_pointer_cast<ResourceLookUpTableInstance>(root->requireInstance(nullptr))
                ->collect(futures, handles, offsets);
            for(auto pos : offsets)
                handles[pos] += reinterpret_cast<ResourceHandle>(handles.data());

            auto future = scheduler.newFutureImpl(0, Closure<void*>{ context(), [](void*) {} }, false);

            scheduler.parallelForImpl(
                size.x * size.y,
                Closure<uint32_t>{ context(),
                                   [handles = std::move(handles), input = std::move(args), size, call = std::move(func),
                                    ref = std::move(root), kernelRef = std::move(kernel)](const uint32_t idx) {
                                       const auto address = call->lookUpSync();

                                       TaskContextImplEx ctxEx{ { { idx % size.x, idx / size.x, 0 },
                                                                  reinterpret_cast<ResourceHandle>(handles.data()) },
                                                                input };
                                       auto&& ctx = ctxEx.ctx;

                                       for(; ctx.taskIndex.z < size.z; ++ctx.taskIndex.z) {
                                           static_cast<KernelProtocol>(address)(reinterpret_cast<TaskContext>(&ctx));
                                       }
                                   } },
                { futures.data(), futures.size() }, future);

            return Future<void>{ std::move(future) };
        }
        SharedPtr<Resource> createBuffer(const size_t size, const size_t alignment, Function<void, Ptr> prepare) override {
            return makeSharedObject<Buffer>(context(), size, alignment, std::move(prepare));
        }
        SharedPtr<ResourceLookUpTable> createResourceLUT(DynamicArray<SharedPtr<Resource>> resources) override {
            return makeSharedObject<ResourceLookUpTableImpl>(context(), std::move(resources));
        }
        SharedPtr<TiledOutput> createTiledOutput(const size_t size, const size_t alignment) override {
            return makeSharedObject<TiledBuffer>(context(), size, alignment);
        }
        [[nodiscard]] const DynamicArray<Context*>& enumerateContexts() const override {
            return mContexts;
        }
    };

    class ModuleImpl final : public Module {
    private:
        String mPath;
        Optional<LinkableProgram> mRuntime;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path) : Module(context), mPath(path, context.getAllocator()) {
            if(!llvm::llvm_is_multithreaded())
                context.getErrorHandler().raiseException("LLVM should be compiled with multi-threading flag.",
                                                         PIPER_SOURCE_LOCATION());
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            mRuntime = context.getPITUManager()
                           .loadPITU(String{ path, context.getAllocator() } + "/Kernel.bc")
                           .getSync()
                           ->generateLinkable({ { "LLVM IR" } });
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Accelerator") {
                return context().getScheduler().value(eastl::static_shared_pointer_cast<Object>(
                    makeSharedObject<ParallelAccelerator>(context(), mRuntime.value())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
        ~ModuleImpl() noexcept override {
            llvm::llvm_shutdown();
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
