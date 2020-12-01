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
#include "../../../STL/UniquePtr.hpp"
#include <new>
#include <utility>
#pragma warning(push, 0)
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
// use LLJIT
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#pragma warning(pop)
#define NOMINMAX
#include "../../../STL/USet.hpp"

#include <Windows.h>

namespace Piper {
    static CommandQueue currentThreadID() {
        return GetCurrentThreadId();
    }

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

    class LLVMProgram final : public RunnableProgram {
    private:
        std::unique_ptr<llvm::orc::LLJIT> mJIT;
        using KernelFunction = void(PIPER_CC*)(uint32_t idx, const std::byte* payload);
        KernelFunction mFunction, mUnroll;

    public:
        LLVMProgram(PiperContext& context, std::unique_ptr<llvm::orc::LLJIT> JIT, const String& entry)
            : RunnableProgram(context), mJIT(std::move(JIT)) {
            mFunction = static_cast<KernelFunction>(lookup(entry));
            mUnroll = static_cast<KernelFunction>(lookup(entry + "_unroll"));
        }
        void run(const uint32_t idx, const std::byte* payload) const {
            mFunction(idx, payload);
        }
        void runUnroll(const uint32_t idx, const std::byte* payload) const {
            mUnroll(idx, payload);
        }
        void* lookup(const String& symbol) override {
            return reinterpret_cast<void*>(
                getLLVMResult(context(), PIPER_SOURCE_LOCATION(), mJIT->lookup(symbol.c_str())).getAddress());
        }
    };

    class ResourceImpl final : public Resource {
    private:
        SharedPtr<FutureImpl> mFuture;

    public:
        ResourceImpl(PiperContext& context, const ResourceHandle handle) : Resource(context, handle) {}
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
        DynamicArray<SharedPtr<ResourceImpl>> mInput;
        DynamicArray<SharedPtr<ResourceImpl>> mOutput;

    public:
        explicit ResourceBindingImpl(PiperContext& context)
            : ResourceBinding(context), mInput(context.getAllocator()), mOutput(context.getAllocator()) {}
        void addInput(const SharedPtr<Resource>& resource) override {
            auto res = eastl::dynamic_pointer_cast<ResourceImpl>(resource);
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            mInput.push_back(std::move(res));
        }
        void addOutput(const SharedPtr<Resource>& resource) override {
            auto res = eastl::dynamic_pointer_cast<ResourceImpl>(std::move(resource));
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            mOutput.push_back(std::move(res));
        }
        [[nodiscard]] DynamicArray<SharedPtr<FutureImpl>> getInput() const {
            DynamicArray<SharedPtr<FutureImpl>> input{ context().getAllocator() };
            input.reserve(mInput.size());
            for(auto&& in : mInput)
                input.push_back(in->getFuture());
            return input;
        }
        void makeDirty(const SharedPtr<FutureImpl>& newFuture) {
            for(auto&& output : mOutput)
                output->setFuture(newFuture);
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

    // TODO:lazy allocation
    class BufferImpl final : public Buffer, public eastl::enable_shared_from_this<BufferImpl> {
    private:
        Ptr mData;
        size_t mSize;
        Allocator& mAllocator;
        SharedPtr<ResourceImpl> mResource;

    public:
        BufferImpl(PiperContext& context, const Ptr data, const size_t size, Allocator& allocator, SharedPtr<ResourceImpl> res)
            : Buffer(context), mData(data), mSize(size), mAllocator(allocator), mResource(std::move(res)) {}
        size_t size() const noexcept override {
            return mSize;
        }
        void upload(Future<DataHolder> data) override {
            auto& scheduler = context().getScheduler();
            const auto res = scheduler.newFutureImpl(0, false);
            SharedPtr<FutureImpl> dep[] = { mResource->getFuture(), data.raw() };
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [src = std::move(data), data = shared_from_this()] {
                                               memcpy(reinterpret_cast<void*>(data->mData), static_cast<void*>(src.get().get()),
                                                      data->mSize);
                                           } },
                                Span<SharedPtr<FutureImpl>>{ std::begin(dep), std::end(dep) }, res);
            mResource->setFuture(res);
        }
        Future<DynamicArray<std::byte>> download() const override {
            auto& scheduler = context().getScheduler();
            return scheduler.spawn(
                [thisBuffer = shared_from_this()](PlaceHolder) {
                    const auto beg = reinterpret_cast<const std::byte*>(thisBuffer->mData);
                    const auto end = beg + thisBuffer->mSize;
                    return DynamicArray<std::byte>{ beg, end, thisBuffer->context().getAllocator() };
                },
                Future<void>{ mResource->getFuture() });
        }
        void reset() override {
            auto& scheduler = context().getScheduler();
            const auto res = scheduler.newFutureImpl(0, false);
            SharedPtr<FutureImpl> dep[] = { mResource->getFuture() };
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [thisData = shared_from_this()] {
                                               memset(reinterpret_cast<void*>(thisData->mData), 0x00, thisData->mSize);
                                           } },
                                Span<SharedPtr<FutureImpl>>{ std::begin(dep), std::end(dep) }, res);
            mResource->setFuture(res);
        }
        SharedPtr<Resource> ref() const override {
            return eastl::static_shared_pointer_cast<Resource>(mResource);
        }
    };

    static void parseNative(PiperContext& context, const DynamicArray<std::byte>& data, UMap<String, void*>& symbol) {
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
            auto func = reinterpret_cast<void*>(*reinterpret_cast<const uint64_t*>(ptr + 1));
            ptr += 9;
            symbol.insert({ std::move(name), func });
        }
    }

    class ParallelAccelerator final : public Accelerator {
    private:
        static constexpr uint32_t chunkSize = 1024;

    public:
        explicit ParallelAccelerator(PiperContext& context) : Accelerator(context) {}
        [[nodiscard]] Span<const CString> getSupportedLinkableFormat() const override {
            // about LLVM IR Version: https://llvm.org/docs/DeveloperPolicy.html#ir-backwards-compatibility
            static CString format[] = { "LLVM IR", "Native" };
            return Span<const CString>{ format };
        }
        [[nodiscard]] SharedPtr<ResourceBinding> createResourceBinding() const override {
            return eastl::static_shared_pointer_cast<ResourceBinding>(makeSharedObject<ResourceBindingImpl>(context()));
        }
        [[nodiscard]] SharedPtr<Payload> createPayloadImpl() const override {
            return eastl::static_shared_pointer_cast<Payload>(makeSharedObject<PayloadImpl>(context()));
        }
        [[nodiscard]] SharedPtr<Resource> createResource(const ResourceHandle handle) const override {
            return eastl::static_shared_pointer_cast<Resource>(makeSharedObject<ResourceImpl>(context(), handle));
        }
        Future<SharedPtr<RunnableProgram>> compileKernel(const Span<Future<LinkableProgram>>& linkable,
                                                         const String& entry) override {
            DynamicArray<Future<std::unique_ptr<llvm::Module>>> modules{ context().getAllocator() };
            USet<size_t> hashPool{ context().getAllocator() };
            UMap<String, void*> nativeSymbol{ context().getAllocator() };
            auto& scheduler = context().getScheduler();
            auto llCtx = std::make_unique<llvm::LLVMContext>();
            // TODO:parallel load?
            for(auto&& unit : linkable) {
                // TODO:concurrency
                unit.wait();

                const auto beg = reinterpret_cast<CString>(unit.get().exchange.data());
                // TODO:use StringView
                const auto data = std::string_view{ beg, unit.get().exchange.size() };
                if(!hashPool.insert(std::hash<std::string_view>{}(data)).second)
                    continue;

                if(unit.get().format == std::string_view{ "LLVM IR" }) {
                    // TODO:concurrency
                    // modules.emplace_back(std::move(scheduler.spawn(
                    //    [ctx = &context(), llvmctx = llctx.get()](const Future<LinkableProgram>& linkable) {
                    auto stage = context().getErrorHandler().enterStage("parse LLVM Bitcode", PIPER_SOURCE_LOCATION());
                    auto res = llvm::parseBitcodeFile(
                        llvm::MemoryBufferRef{
                            toStringRef(llvm::ArrayRef<uint8_t>{ reinterpret_cast<const uint8_t*>(unit.get().exchange.data()),
                                                                 unit.get().exchange.size() }),
                            "bitcode data" },
                        *llCtx);
                    modules.push_back(scheduler.value(getLLVMResult(context(), PIPER_SOURCE_LOCATION(), std::move(res))));
                } else if(unit.get().format == std::string_view{ "Native" }) {
                    parseNative(context(), unit.get().exchange, nativeSymbol);
                } else
                    context().getErrorHandler().raiseException("Unsupported linkable format " +
                                                                   String{ unit.get().format, context().getAllocator() },
                                                               PIPER_SOURCE_LOCATION());
            }
            // TODO:reduce Module cloning by std::move
            // TODO:remove unused functions/maintain symbol reference
            const bool entryInNative = nativeSymbol.count(entry);
            auto kernel = scheduler.spawn(
                [ctx = &context(), llvmCtx = llCtx.get(), entry,
                 entryInNative](const DynamicArray<std::unique_ptr<llvm::Module>>& units) {
                    auto& errorHandler = ctx->getErrorHandler();
                    auto stage = errorHandler.enterStage("link LLVM modules", PIPER_SOURCE_LOCATION());
                    auto module = std::make_unique<llvm::Module>("LLVM_kernel", *llvmCtx);
                    for(auto&& unit : units)
                        llvm::Linker::linkModules(*module, llvm::CloneModule(*unit));

                    auto* func = module->getFunction(llvm::StringRef{ entry.data(), entry.size() });
                    if(!func) {
                        if(entryInNative) {
                            llvm::Type* params[] = { llvm::Type::getInt32Ty(*llvmCtx), llvm::Type::getInt8PtrTy(*llvmCtx) };
                            auto* FT = llvm::FunctionType::get(llvm::Type::getVoidTy(*llvmCtx),
                                                               llvm::ArrayRef<llvm::Type*>{ params, 2 }, false);
                            func = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                                          llvm::Twine{ llvm::StringRef{ entry.data(), entry.size() } }, *module);
                        } else
                            errorHandler.raiseException("Undefined entry " + entry, PIPER_SOURCE_LOCATION());
                    }
                    {
                        auto* payload = func->getArg(1);
                        payload->addAttr(llvm::Attribute::NonNull);
                        payload->addAttr(llvm::Attribute::ReadOnly);
                    }

                    // TODO:check interface
                    stage.next("build for-loop unroll helper", PIPER_SOURCE_LOCATION());
                    {
                        llvm::Type* argTypes[] = { llvm::Type::getInt32Ty(*llvmCtx), func->getFunctionType()->getParamType(1) };
                        auto* sig = llvm::FunctionType::get(llvm::Type::getVoidTy(*llvmCtx), argTypes, false);
                        auto* unroll = llvm::Function::Create(sig, llvm::GlobalValue::LinkageTypes::ExternalLinkage,
                                                              (entry + "_unroll").c_str(), *module);
                        auto* body = llvm::BasicBlock::Create(*llvmCtx, "body", unroll);
                        llvm::IRBuilder<> builder{ body };
                        auto *idx = unroll->getArg(0), *payload = unroll->getArg(1);
                        payload->addAttr(llvm::Attribute::NonNull);
                        payload->addAttr(llvm::Attribute::ReadOnly);

                        auto* loop = llvm::BasicBlock::Create(*llvmCtx, "loop", unroll);
                        auto* pre = builder.GetInsertBlock();
                        builder.CreateBr(loop);
                        builder.SetInsertPoint(loop);
                        auto* offset = builder.CreatePHI(idx->getType(), 2, "offset");
                        offset->addIncoming(llvm::ConstantInt::get(idx->getType(), 0), pre);

                        llvm::Value* args[] = { builder.CreateAdd(idx, offset), payload };
                        builder.CreateCall(func, args);

                        auto* step = llvm::ConstantInt::get(idx->getType(), 1);
                        auto* next = builder.CreateAdd(offset, step, "next");

                        auto* cond = builder.CreateICmpULT(next, llvm::ConstantInt::get(idx->getType(), chunkSize), "cond");

                        auto* loopEnd = builder.GetInsertBlock();
                        offset->addIncoming(next, loopEnd);

                        auto* after = llvm::BasicBlock::Create(*llvmCtx, "after", unroll);
                        builder.CreateCondBr(cond, loop, after);

                        builder.SetInsertPoint(after);
                        builder.CreateRetVoid();
                    }

                    stage.next("verify kernel", PIPER_SOURCE_LOCATION());

                    LLVMLoggerWrapper reporter{ *ctx, PIPER_SOURCE_LOCATION() };
                    // NOTICE: return true if the module is broken.
                    if(llvm::verifyModule(*module, &reporter)) {
                        reporter.flushToLogger();
                        errorHandler.raiseException("Found some errors in module", PIPER_SOURCE_LOCATION());
                    }

                    llvm::legacy::PassManager manager;
                    std::string output;
                    llvm::raw_string_ostream out(output);
                    manager.add(llvm::createPrintModulePass(out));
                    manager.run(*module);

                    out.flush();
                    if(ctx->getLogger().allow(LogLevel::Debug))
                        ctx->getLogger().record(LogLevel::Debug, output.c_str(), PIPER_SOURCE_LOCATION());

                    return module;
                },
                scheduler.wrap(modules));
            return scheduler.spawn(
                [ctx = &context(), llvmCtx = std::move(llCtx), entry, nativeSymbol, this](std::unique_ptr<llvm::Module> mod) {
                    // TODO:LLVM use fake host triple,use true host triple to initialize JITTargetMachineBuilder
                    auto JTMB = getLLVMResult(*ctx, PIPER_SOURCE_LOCATION(), llvm::orc::JITTargetMachineBuilder::detectHost());

                    // TODO:test settings
                    // TODO:FP Precise/Atomic
                    JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);
                    JTMB.setRelocationModel(llvm::Reloc::PIC_);
                    JTMB.setCodeModel(llvm::CodeModel::Small);

                    auto& options = JTMB.getOptions();
                    options.EmulatedTLS = false;
                    options.TLSSize = 0;

                    // TODO:shared engine
                    auto engine = getLLVMResult(*ctx, PIPER_SOURCE_LOCATION(),
                                                std::move(llvm::orc::LLJITBuilder{}
                                                              .setNumCompileThreads(std::thread::hardware_concurrency())
                                                              .setJITTargetMachineBuilder(std::move(JTMB)))
                                                    .create());

                    mod->setDataLayout(engine->getDataLayout());

                    if(auto err = engine->addIRModule(llvm::orc::ThreadSafeModule{
                           std::move(mod), std::move(const_cast<std::remove_const_t<decltype(llvmCtx)>&>(llvmCtx)) }))
                        ctx->getErrorHandler().raiseException(llvm::toString(std::move(err)).c_str(), PIPER_SOURCE_LOCATION());

                    {
                        const auto flag =
                            llvm::JITSymbolFlags::Callable | llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Absolute;
                        llvm::orc::SymbolMap map = {
#define PIPER_MATH_KERNEL_FUNC(name)                                            \
    {                                                                           \
        engine->getExecutionSession().intern(#name), llvm::JITEvaluatedSymbol { \
            reinterpret_cast<llvm::JITTargetAddress>(name), flag                \
        }                                                                       \
    }
                            PIPER_MATH_KERNEL_FUNC(cosf),  PIPER_MATH_KERNEL_FUNC(sinf),   PIPER_MATH_KERNEL_FUNC(fabsf),
                            PIPER_MATH_KERNEL_FUNC(fmaxf), PIPER_MATH_KERNEL_FUNC(fminf),  PIPER_MATH_KERNEL_FUNC(sqrtf),
                            PIPER_MATH_KERNEL_FUNC(cbrtf), PIPER_MATH_KERNEL_FUNC(hypotf),
#undef PIPER_MATH_KERNEL_FUNC
                        };
                        for(const auto& symbol : nativeSymbol)
                            map.insert(std::make_pair(
                                engine->getExecutionSession().intern(llvm::StringRef{ symbol.first.data(), symbol.first.size() }),
                                llvm::JITEvaluatedSymbol{ reinterpret_cast<llvm::JITTargetAddress>(symbol.second), flag }));
                        static char id;
                        auto MU = std::make_unique<llvm::orc::AbsoluteSymbolsMaterializationUnit>(
                            map, reinterpret_cast<llvm::orc::VModuleKey>(&id));

                        if(auto err = engine->define(std::move(MU)))
                            ctx->getErrorHandler().raiseException(llvm::toString(std::move(err)).c_str(),
                                                                  PIPER_SOURCE_LOCATION());
                    }

                    return eastl::static_shared_pointer_cast<RunnableProgram>(
                        makeSharedObject<LLVMProgram>(*ctx, std::move(engine), entry));
                },
                std::move(kernel));
        }
        Future<void> runKernel(uint32_t n, const Future<SharedPtr<RunnableProgram>>& kernel,
                               const SharedPtr<Payload>& payload) override {
            // TODO:for small n,run in the thread
            auto& scheduler = context().getScheduler();
            auto* payloadImpl = dynamic_cast<PayloadImpl*>(payload.get());
            auto&& binding = payloadImpl->getResourceBinding();
            // TODO:reduce copy
            auto inputFuture = binding->getInput();
            DynamicArray<Future<void>> input{ context().getAllocator() };
            input.reserve(inputFuture.size());
            for(auto&& in : inputFuture)
                input.push_back(Future<void>{ in });
            auto future = scheduler
                              .parallelFor((n + chunkSize - 1) / chunkSize,
                                           [input = payloadImpl->data(), n](const uint32_t idx,
                                                                            const SharedPtr<RunnableProgram>& func, PlaceHolder) {
                                               auto beg = idx * chunkSize;
                                               const auto end = beg + chunkSize;
                                               auto& kernel = dynamic_cast<LLVMProgram&>(*func);
                                               if(end < n)
                                                   kernel.runUnroll(beg, input.data());
                                               else {
                                                   while(beg < n) {
                                                       kernel.run(beg, input.data());
                                                       ++beg;
                                                   }
                                               }
                                           },
                                           std::move(kernel), scheduler.wrap(input))
                              .raw();
            binding->makeDirty(future);
            return Future<void>{ std::move(future) };
        }
        void apply(Function<void, Context, CommandQueue> func, const SharedPtr<ResourceBinding>& binding) override {
            auto* bind = dynamic_cast<ResourceBindingImpl*>(binding.get());
            auto input = bind->getInput();
            auto& scheduler = context().getScheduler();
            const auto result = scheduler.newFutureImpl(0, false);
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [call = std::move(func)] { call(static_cast<Context>(0), currentThreadID()); } },
                                Span<SharedPtr<FutureImpl>>{ input.data(), input.size() }, result);
            bind->makeDirty(result);
        }
        Future<void> available(const SharedPtr<Resource>& resource) override {
            auto* res = dynamic_cast<ResourceImpl*>(resource.get());
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            return Future<void>{ res->getFuture() };
        }
        SharedPtr<Buffer> createBuffer(const size_t size, const size_t alignment) override {
            auto& allocator = context().getAllocator();
            auto ptr = allocator.alloc(size, alignment);
            return eastl::static_shared_pointer_cast<Buffer>(
                makeSharedObject<BufferImpl>(context(), ptr, size, allocator, makeSharedObject<ResourceImpl>(context(), ptr)));
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path) : Module(context), mPath(path, context.getAllocator()) {
            if(!llvm::llvm_is_multithreaded())
                context.getErrorHandler().raiseException("LLVM should be compiled with multi-threading flag.",
                                                         PIPER_SOURCE_LOCATION());
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Accelerator") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ParallelAccelerator>(context())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
        ~ModuleImpl() noexcept override {
            llvm::llvm_shutdown();
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
