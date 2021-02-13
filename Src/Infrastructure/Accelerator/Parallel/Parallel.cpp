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
#include <llvm/Linker/Linker.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
// use LLJIT
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Support/ManagedStatic.h>
//#define NOMINMAX
//#include <Windows.h>
#pragma warning(pop)

extern "C" void __chkstk();

namespace Piper {
    // TODO: better implementation for cross-platform
    /*
    static CommandQueueHandle currentThreadID() {
        return static_cast<CommandQueueHandle>(GetCurrentThreadId());
    }
    */

    template <typename T>
    auto getLLVMResult(PiperContext& context, const SourceLocation& loc, llvm::Expected<T> value) {
        if(value)
            return std::move(std::move(value).get());
        context.getErrorHandler().raiseException(("LLVM error: " + llvm::toString(value.takeError())).c_str(), loc);
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

    class LLVMProgram final : public RunnableProgramUntyped {
    private:
        std::unique_ptr<llvm::orc::LLJIT> mJIT;
        using KernelFunction = void (*)(const TaskContext& ctx);
        KernelFunction mFunction;

    public:
        LLVMProgram(PiperContext& context, std::unique_ptr<llvm::orc::LLJIT> JIT, const String& entry)
            : RunnableProgramUntyped{ context }, mJIT{ std::move(JIT) } {
            mFunction = static_cast<KernelFunction>(lookup(entry));
        }
        void run(const TaskContext& ctx) const {
            mFunction(ctx);
        }
        void* lookup(const String& symbol) override {
            return reinterpret_cast<void*>(
                getLLVMResult(context(), PIPER_SOURCE_LOCATION(), mJIT->lookup(symbol.c_str())).getAddress());
        }
    };

    class BufferInstance final : public ResourceInstance {
    private:
        Allocator& mAllocator;
        Ptr mPtr;

    public:
        // TODO: lazy allocation
        BufferInstance(PiperContext& context, const size_t size, const size_t alignment)
            : ResourceInstance{ context }, mAllocator{ context.getAllocator() }, mPtr{ mAllocator.alloc(size, alignment) } {}
        ~BufferInstance() override {
            mAllocator.free(mPtr);
        }
        [[nodiscard]] ResourceHandle getHandle() const noexcept override {
            return static_cast<ResourceHandle>(mPtr);
        }
        [[nodiscard]] Ptr data() const noexcept {
            return mPtr;
        }
    };

    class BufferImpl final : public Buffer, public eastl::enable_shared_from_this<BufferImpl> {
    private:
        size_t mSize;
        size_t mAlignment;

    public:
        BufferImpl(PiperContext& context, const size_t size, const size_t alignment)
            : Buffer{ context }, mSize{ size }, mAlignment{ alignment } {}
        [[nodiscard]] ResourceShareMode getShareMode() const noexcept override {
            return ResourceShareMode::Unique;
        }
        void flushBeforeRead(const Instance& dest) const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        }
        [[nodiscard]] Instance instantiateMain() const override {
            return instantiateReference(nullptr);
        }
        [[nodiscard]] Instance instantiateReference(const ContextHandle ctx) const override {
            return Instance{ ctx, makeSharedObject<BufferInstance>(context(), mSize, mAlignment) };
        }
        [[nodiscard]] size_t size() const noexcept override {
            return mSize;
        }
        void upload(Function<void, Ptr> prepare) override {
            auto& scheduler = context().getScheduler();
            auto&& main = requireMain().second;
            const auto res = scheduler.spawn(
                [self = shared_from_this(), &main, func = std::move(prepare)](PlaceHolder) {
                    const auto ptr = dynamic_cast<BufferInstance*>(main.get())->data();
                    // TODO: support async
                    func(ptr);
                },
                Future<void>{ main->getFuture() });
            main->setFuture(res.raw());
        }
        Future<Binary> download() const override {
            auto& scheduler = context().getScheduler();
            auto&& main = requireMain().second;
            return scheduler.spawn(
                [thisBuffer = shared_from_this(), &main](PlaceHolder) {
                    const auto* beg = reinterpret_cast<std::byte*>(dynamic_cast<BufferInstance*>(main.get())->data());
                    const auto* end = beg + thisBuffer->mSize;
                    return Binary{ beg, end, thisBuffer->context().getAllocator() };
                },
                Future<void>{ main->getFuture() });
        }
        void reset() override {
            auto& scheduler = context().getScheduler();
            auto&& main = requireMain().second;
            const auto res = scheduler.spawn(
                [self = shared_from_this(), &main](PlaceHolder) {
                    memset(reinterpret_cast<void*>(dynamic_cast<BufferInstance*>(main.get())->data()), 0x00, self->mSize);
                },
                Future<void>{ main->getFuture() });
            main->setFuture(res.raw());
        }
    };

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

    struct TaskContextEx {
        TaskContext ctx;
        const ArgumentPackage& args;
        const DynamicArray<ResourceView>& resources;
    };

    static void piperGetArgument(const TaskContext& context, const uint32_t index, void* ptr) {
        auto&& ctxEx = *reinterpret_cast<const TaskContextEx*>(&context);
        const auto [offset, size] = ctxEx.args.offset[index];
        memcpy(ptr, ctxEx.args.data.data() + offset, size);
    }
    static void piperGetResourceHandle(const TaskContext& context, const uint32_t index, ResourceHandle& handle) {
        auto&& ctxEx = *reinterpret_cast<const TaskContextEx*>(&context);
        handle = ctxEx.resources[index].resource->require(nullptr)->getHandle();
    }

    class ParallelAccelerator final : public Accelerator {
    private:
        static constexpr uint32_t chunkSize = 1024;
        LinkableProgram mRuntime;

    public:
        explicit ParallelAccelerator(PiperContext& context, LinkableProgram runtime)
            : Accelerator{ context }, mRuntime{ std::move(runtime) } {}
        [[nodiscard]] Span<const CString> getSupportedLinkableFormat() const noexcept override {
            // about LLVM IR Version: https://llvm.org/docs/DeveloperPolicy.html#ir-backwards-compatibility
            static CString format[] = { "LLVM IR", "Native" };
            return Span<const CString>{ format };
        }
        [[nodiscard]] CString getNativePlatform() const noexcept override {
            return "CPU";
        }
        [[nodiscard]] Future<SharedPtr<RunnableProgramUntyped>> compileKernelImpl(const Span<LinkableProgram>& linkable,
                                                                                  const String& entry) override {
            DynamicArray<Future<std::unique_ptr<llvm::Module>>> modules{ context().getAllocator() };
            USet<size_t> hashPool{ context().getAllocator() };
            UMap<String, void*> nativeSymbol{ context().getAllocator() };
            auto& scheduler = context().getScheduler();
            auto llCtx = std::make_unique<llvm::LLVMContext>();

            // TODO:parallel load?
            auto addModule = [&](LinkableProgram& unit) {
                // TODO:concurrency
                unit.exchange.wait();

                if(!hashPool.insert(unit.UID).second)
                    return;

                if(unit.format == "LLVM IR") {
                    // TODO:concurrency
                    // modules.emplace_back(std::move(scheduler.spawn(
                    //    [ctx = &context(), llvmctx = llctx.get()](const Future<LinkableProgram>& linkable) {
                    auto stage = context().getErrorHandler().enterStage("parse LLVM Bitcode", PIPER_SOURCE_LOCATION());
                    auto res = llvm::parseBitcodeFile(
                        llvm::MemoryBufferRef{ toStringRef(llvm::ArrayRef<uint8_t>{
                                                   reinterpret_cast<const uint8_t*>(unit.exchange.getUnsafe().data()),
                                                   unit.exchange.getUnsafe().size() }),
                                               "bitcode data" },
                        *llCtx);
                    modules.push_back(scheduler.value(getLLVMResult(context(), PIPER_SOURCE_LOCATION(), std::move(res))));
                } else if(unit.format == "Native") {
                    parseNative(context(), unit.exchange.getUnsafe(), nativeSymbol);
                } else
                    context().getErrorHandler().raiseException("Unsupported linkable format " +
                                                                   String{ unit.format, context().getAllocator() },
                                                               PIPER_SOURCE_LOCATION());
            };
            addModule(mRuntime);
            for(auto&& unit : linkable)
                addModule(unit);

            // TODO:reduce Module cloning by std::move
            // TODO:remove unused functions/maintain symbol reference
            const bool entryInNative = nativeSymbol.count(entry);
            auto kernel = scheduler.spawn(
                [ctx = &context(), llvmCtx = std::move(llCtx), entry,
                 entryInNative](const DynamicArray<std::unique_ptr<llvm::Module>>& units) {
                    auto& errorHandler = ctx->getErrorHandler();
                    auto stage = errorHandler.enterStage("link LLVM modules", PIPER_SOURCE_LOCATION());
                    auto module = std::make_unique<llvm::Module>("LLVM_kernel", *llvmCtx);
                    for(auto&& unit : units)
                        llvm::Linker::linkModules(*module, llvm::CloneModule(*unit));

                    // TODO:check interface

                    stage.next("verify kernel", PIPER_SOURCE_LOCATION());

                    LLVMLoggerWrapper reporter{ *ctx, PIPER_SOURCE_LOCATION() };
                    // NOTICE: return true if the module is broken.
                    if(llvm::verifyModule(*module, &reporter)) {
                        reporter.flushToLogger();
                        errorHandler.raiseException("Found some errors in module", PIPER_SOURCE_LOCATION());
                    }
                    /*
                    llvm::legacy::PassManager manager;
                    std::string output;
                    llvm::raw_string_ostream out(output);
                    manager.add(llvm::createPrintModulePass(out));
                    manager.run(*module);
                    out.flush();
                    if(ctx->getLogger().allow(LogLevel::Debug))
                        ctx->getLogger().record(LogLevel::Debug, output.c_str(), PIPER_SOURCE_LOCATION());
                        */

                    return llvm::orc::ThreadSafeModule{ std::move(module),
                                                        std::move(const_cast<std::remove_const_t<decltype(llvmCtx)>&>(llvmCtx)) };
                },
                scheduler.wrap(modules));
            return scheduler.spawn(
                [ctx = &context(), entry, nativeSymbol, this](llvm::orc::ThreadSafeModule mod) {
                    auto stage = ctx->getErrorHandler().enterStage("build JIT", PIPER_SOURCE_LOCATION());
                    // TODO:LLVM use fake host triple,use true host triple to initialize JITTargetMachineBuilder
                    auto JTMB = getLLVMResult(*ctx, PIPER_SOURCE_LOCATION(), llvm::orc::JITTargetMachineBuilder::detectHost());

                    // TODO:test settings
                    // TODO:FP Precise
                    JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);
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

                    mod.getModuleUnlocked()->setDataLayout(engine->getDataLayout());

                    if(auto err = engine->addIRModule(std::move(mod)))
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
                            // Math functions
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
                            PIPER_NATIVE_FUNC(piperGetArgument), PIPER_NATIVE_FUNC(piperGetResourceHandle),
                            // MSVC internal functions
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

                    return eastl::static_shared_pointer_cast<RunnableProgramUntyped>(
                        makeSharedObject<LLVMProgram>(*ctx, std::move(engine), entry));
                },
                std::move(kernel));
        }
        Future<void> launchKernelImpl(const Dim3& grid, const Dim3& block,
                                      const Future<SharedPtr<RunnableProgramUntyped>>& kernel,
                                      const DynamicArray<ResourceView>& resources, const ArgumentPackage& args) override {
            // TODO: for small n,run in this thread
            const auto blockCount = grid.x * grid.y * grid.z;
            const auto taskPerBlock = block.x * block.y * block.z;

            auto& scheduler = context().getScheduler();
            // TODO: reduce copy
            DynamicArray<Future<void>> input{ context().getAllocator() };
            for(auto&& [resource, access] : resources)
                input.push_back(Future<void>{ resource->access() });

            auto future =
                scheduler
                    .parallelFor(
                        blockCount,
                        [taskPerBlock, resources, input = args, grid,
                         block](const uint32_t idx, const SharedPtr<RunnableProgramUntyped>& func, PlaceHolder) {
                            auto& program = dynamic_cast<LLVMProgram&>(*func);

                            TaskContextEx ctxEx{ { grid,
                                                   { idx / grid.z / grid.y, idx / grid.z % grid.y, idx % grid.z },
                                                   block,
                                                   idx,
                                                   idx * taskPerBlock,
                                                   0,
                                                   { 0, 0, 0 } },
                                                 input,
                                                 resources };
                            auto&& ctx = ctxEx.ctx;

                            // TODO: support unroll
                            for(auto& i = ctx.blockIndex.x = 0; i < block.x; ++i)
                                for(auto& j = ctx.blockIndex.y = 0; j < block.y; ++j)
                                    for(auto& k = ctx.blockIndex.z = 0; k < block.z; ++k, ++ctx.blockLinearIndex, ++ctx.index) {
                                        program.run(ctx);
                                    }
                        },
                        kernel, scheduler.wrap(input))
                    .raw();
            // TODO: better interface
            for(auto&& [resource, access] : resources)
                if(access == ResourceAccessMode::ReadWrite)
                    resource->makeDirty(future);
            return Future<void>{ std::move(future) };
        }
        SharedPtr<Buffer> createBuffer(const size_t size, const size_t alignment) override {
            return eastl::static_shared_pointer_cast<Buffer>(makeSharedObject<BufferImpl>(context(), size, alignment));
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
