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
#define NOMINMAX
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/Concurrency.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
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
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
// use LLJIT
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#pragma warning(pop)
#include <Windows.h>

namespace Piper {
    static CommandQueue currentThreadID() {
        return GetCurrentThreadId();
    }

    // TODO:set LLVM Allocator
    template <typename T>
    auto getLLVMResult(PiperContext& context, llvm::Expected<T> value) {
        if(value)
            return std::move(std::move(value).get());
        context.getErrorHandler().raiseException(("LLVM error " + llvm::toString(value.takeError())).c_str(),
                                                 PIPER_SOURCE_LOCATION());
    }

    class LLVMLoggerWrapper : public llvm::raw_ostream {
    private:
        Logger& mLogger;
        SourceLocation mLocation;
        Vector<char> mData;

    public:
        explicit LLVMLoggerWrapper(PiperContext& context, const SourceLocation& location)
            : raw_ostream(true), mLogger(context.getLogger()), mLocation(location), mData(context.getAllocator()) {}
        uint64_t current_pos() const override {
            return mData.size();
        }
        void write_impl(const char* ptr, const size_t size) override {
            mData.insert(mData.cend(), ptr, ptr + size);
        }
        void flush() {
            if(!mData.empty() && mLogger.allow(LogLevel::Error)) {
                mLogger.record(LogLevel::Error, StringView{ mData.data(), mData.size() }, mLocation);
                mLogger.flush();
            }
        }
    };

    class LLVMProgram final : public RunnableProgram {
    private:
        std::unique_ptr<llvm::orc::LLJIT> mJIT;
        using KernelFunction = void (*)(uint32_t idx, const std::byte* payload);
        KernelFunction mFunction, mUnroll;

    public:
        LLVMProgram(PiperContext& context, std::unique_ptr<llvm::orc::LLJIT> JIT, const String& entry)
            : RunnableProgram(context), mJIT(std::move(JIT)),
              mFunction(reinterpret_cast<KernelFunction>(getLLVMResult(context, mJIT->lookup(entry.c_str())).getAddress())),
              mUnroll(reinterpret_cast<KernelFunction>(
                  getLLVMResult(context, mJIT->lookup((entry + "_unroll").c_str())).getAddress())) {
            if(!mFunction)
                context.getErrorHandler().raiseException("Undefined entry " + entry, PIPER_SOURCE_LOCATION());
        }
        void run(const uint32_t idx, const std::byte* payload) {
            mFunction(idx, payload);
        }
        void runUnroll(const uint32_t idx, const std::byte* payload) {
            mUnroll(idx, payload);
        }
    };

    class ResourceImpl final : public Resource {
    private:
        SharedPtr<FutureImpl> mFuture;

    public:
        ResourceImpl(PiperContext& context, const ResourceHandle handle) : Resource(context, handle) {}
        SharedPtr<FutureImpl> getFuture() const {
            return mFuture;
        }
        void setFuture(SharedPtr<FutureImpl> future) {
            // TODO:formal check
            if(mFuture.unique() && !mFuture->ready())
                throw;
            mFuture = std::move(future);
        }
    };

    class ResourceBindingImpl final : public ResourceBinding {
    private:
        Vector<SharedPtr<ResourceImpl>> mInput;
        Vector<SharedPtr<ResourceImpl>> mOutput;

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
        Vector<SharedPtr<FutureImpl>> getInput() const {
            Vector<SharedPtr<FutureImpl>> input{ context().getAllocator() };
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

    class ArgumentImpl final : public Argument {
    private:
        Vector<std::byte> mArgument;
        SharedPtr<ResourceBindingImpl> mResourceBinding;

    public:
        explicit ArgumentImpl(PiperContext& context)
            : Argument(context), mArgument(STLAllocator{ context.getAllocator() }),
              mResourceBinding(makeSharedObject<ResourceBindingImpl>(context)) {}
        void appendInput(const SharedPtr<Resource>& resource) override {
            Argument::append(resource->getHandle());
            addExtraInput(resource);
        }
        void appendOutput(const SharedPtr<Resource>& resource) override {
            Argument::append(resource->getHandle());
            addExtraOutput(resource);
        }
        void append(const void* data, const size_t size, const size_t alignment) override {
            const auto rem = mArgument.size() % alignment;
            if(rem) {
                auto& logger = context().getLogger();
                if(logger.allow(LogLevel::Warning))
                    logger.record(LogLevel::Warning, "Inefficient payload layout", PIPER_SOURCE_LOCATION());
                mArgument.insert(mArgument.cend(), alignment - rem, std::byte{ 0 });
            }
            const auto beg = static_cast<const std::byte*>(data);
            const auto end = beg + size;
            mArgument.insert(mArgument.cend(), beg, end);
        }
        void appendInputOutput(const SharedPtr<Resource>& resource) override {
            Argument::append(resource->getHandle());
            addExtraInput(resource);
            addExtraOutput(resource);
        }
        void addExtraInput(const SharedPtr<Resource>& resource) override {
            mResourceBinding->addInput(resource);
        }
        void addExtraOutput(const SharedPtr<Resource>& resource) override {
            mResourceBinding->addOutput(resource);
        }
        Vector<std::byte> getArgument() const {
            return mArgument;
        }
        SharedPtr<ResourceBindingImpl> getResourceBinding() const {
            return mResourceBinding;
        }
    };

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
            auto res = scheduler.newFutureImpl(0, false);
            auto dep = std::initializer_list<const SharedPtr<FutureImpl>>{ mResource->getFuture(), data.raw() };
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [dest = mData, src = std::move(data), size = mSize, rc = shared_from_this()] {
                                               memcpy(reinterpret_cast<void*>(dest), static_cast<void*>(src.get().get()), size);
                                           } },
                                Span<const SharedPtr<FutureImpl>>{ dep.begin(), dep.end() }, res);
            mResource->setFuture(res);
        }
        Future<Vector<std::byte>> download() const override {
            auto& scheduler = context().getScheduler();
            return scheduler.spawn(
                [thisBuffer = shared_from_this()](const Future<void>&) {
                    const auto beg = reinterpret_cast<const std::byte*>(thisBuffer->mData);
                    const auto end = beg + thisBuffer->mSize;
                    return Vector<std::byte>{ beg, end, thisBuffer->context().getAllocator() };
                },
                Future<void>{ mResource->getFuture() });
        }
        void reset() override {
            auto& scheduler = context().getScheduler();
            auto res = scheduler.newFutureImpl(0, false);
            auto dep = std::initializer_list<const SharedPtr<FutureImpl>>{ mResource->getFuture() };
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [thisData = shared_from_this()] {
                                               memset(reinterpret_cast<void*>(thisData->mData), 0x00, thisData->mSize);
                                           } },
                                Span<const SharedPtr<FutureImpl>>{ dep.begin(), dep.end() }, res);
            mResource->setFuture(res);
        }
        SharedPtr<Resource> ref() const override {
            return eastl::static_shared_pointer_cast<Resource>(mResource);
        }
    };

    class ParallelAccelerator final : public Accelerator {
    private:
        CString mSupportedLinkable;
        static constexpr uint32_t chunkSize = 1024;

    public:
        // TODO:support FFI,DLL
        // TODO:LLVM IR Version
        explicit ParallelAccelerator(PiperContext& context) : Accelerator(context), mSupportedLinkable("LLVM IR") {}
        Span<const CString> getSupportedLinkableFormat() const override {
            return Span<const CString>{ &mSupportedLinkable, 1 };
        }
        SharedPtr<ResourceBinding> createResourceBinding() const override {
            return eastl::static_shared_pointer_cast<ResourceBinding>(makeSharedObject<ResourceBindingImpl>(context()));
        }
        SharedPtr<Argument> createArgument() const override {
            return eastl::static_shared_pointer_cast<Argument>(makeSharedObject<ArgumentImpl>(context()));
        }
        SharedPtr<Resource> createResource(const ResourceHandle handle) const override {
            return eastl::static_shared_pointer_cast<Resource>(makeSharedObject<ResourceImpl>(context(), handle));
        }
        Future<SharedPtr<RunnableProgram>> compileKernel(const Vector<Future<Vector<std::byte>>>& linkable,
                                                         const String& entry) override {
            Vector<Future<std::unique_ptr<llvm::Module>>> modules{ context().getAllocator() };
            modules.reserve(linkable.size());
            auto& scheduler = context().getScheduler();
            auto llctx = std::make_unique<llvm::LLVMContext>();
            // TODO:use parallel_for?
            for(auto&& unit : linkable)
                modules.emplace_back(std::move(scheduler.spawn(
                    [ctx = &context(), llvmctx = llctx.get()](const Future<Vector<std::byte>>& data) {
                        auto stage = ctx->getErrorHandler().enterStageStatic("parse LLVM Bitcode", PIPER_SOURCE_LOCATION());
                        auto& bitcode = data.get();
                        auto res = llvm::parseBitcodeFile(
                            llvm::MemoryBufferRef{ toStringRef(llvm::ArrayRef<uint8_t>{
                                                       reinterpret_cast<const uint8_t*>(bitcode.data()), bitcode.size() }),
                                                   "bitcode data" },
                            *llvmctx);
                        if(res)
                            return std::move(res.get());
                        auto error = toString(res.takeError());
                        ctx->getErrorHandler().raiseException(StringView{ error.c_str(), error.size() }, PIPER_SOURCE_LOCATION());
                    },
                    unit)));
            // TODO:reduce Module cloning by std::move
            auto kernel = scheduler.spawn(
                [ctx = &context(), llvmctx = llctx.get(), entry](const Future<Vector<std::unique_ptr<llvm::Module>>>& mods) {
                    auto& errorHandler = ctx->getErrorHandler();
                    auto stage = errorHandler.enterStageStatic("link LLVM modules", PIPER_SOURCE_LOCATION());
                    auto& units = mods.get();
                    auto kernel = std::make_unique<llvm::Module>("LLVM kernel", *llvmctx);
                    for(auto&& unit : units)
                        llvm::Linker::linkModules(*kernel, llvm::CloneModule(*unit));

                    auto func = kernel->getFunction(llvm::StringRef{ entry.data(), entry.size() });
                    if(!func)
                        errorHandler.raiseException("Undefined entry " + entry, PIPER_SOURCE_LOCATION());
                    {
                        auto payload = func->getArg(1);
                        payload->addAttr(llvm::Attribute::NonNull);
                        payload->addAttr(llvm::Attribute::ReadOnly);
                    }
                    // TODO:inline hint?
                    func->addFnAttr(llvm::Attribute::AlwaysInline);

                    // TODO:check interface
                    stage.switchToStatic("build for-loop unroll helper", PIPER_SOURCE_LOCATION());
                    {
                        llvm::Type* argTypes[] = { llvm::Type::getInt32Ty(*llvmctx), func->getFunctionType()->getParamType(1) };
                        auto sig = llvm::FunctionType::get(llvm::Type::getVoidTy(*llvmctx), argTypes, false);
                        auto unroll = llvm::Function::Create(sig, llvm::GlobalValue::LinkageTypes::ExternalLinkage,
                                                             (entry + "_unroll").c_str(), *kernel);
                        auto body = llvm::BasicBlock::Create(*llvmctx, "body", unroll);
                        llvm::IRBuilder<> builder{ body };
                        auto idx = unroll->getArg(0), payload = unroll->getArg(1);
                        payload->addAttr(llvm::Attribute::NonNull);
                        payload->addAttr(llvm::Attribute::ReadOnly);

                        auto loop = llvm::BasicBlock::Create(*llvmctx, "loop", unroll);
                        auto pre = builder.GetInsertBlock();
                        builder.CreateBr(loop);
                        builder.SetInsertPoint(loop);
                        auto offset = builder.CreatePHI(idx->getType(), 2, "offset");
                        offset->addIncoming(llvm::ConstantInt::get(idx->getType(), 0), pre);

                        llvm::Value* args[] = { builder.CreateAdd(idx, offset), payload };
                        builder.CreateCall(func, args);

                        auto step = llvm::ConstantInt::get(idx->getType(), 1);
                        auto next = builder.CreateAdd(offset, step, "next");

                        auto cond = builder.CreateICmpULT(next, llvm::ConstantInt::get(idx->getType(), chunkSize), "cond");

                        auto loopEnd = builder.GetInsertBlock();
                        offset->addIncoming(next, loopEnd);

                        auto after = llvm::BasicBlock::Create(*llvmctx, "after", unroll);
                        builder.CreateCondBr(cond, loop, after);

                        builder.SetInsertPoint(after);
                        builder.CreateRetVoid();
                    }

                    // kernel->dump();

                    stage.switchToStatic("verify kernel", PIPER_SOURCE_LOCATION());

                    LLVMLoggerWrapper reporter{ *ctx, PIPER_SOURCE_LOCATION() };
                    // NOTICE: return true if the module is broken.
                    if(llvm::verifyModule(*kernel, &reporter)) {
                        reporter.flush();
                        errorHandler.raiseException("Found some errors in module", PIPER_SOURCE_LOCATION());
                    }

                    return std::move(kernel);
                },
                scheduler.wrap(modules));
            return scheduler.spawn(
                [entry, ctx = &context(), llvmctx = std::move(llctx)](Future<std::unique_ptr<llvm::Module>> func) {
                    auto mod = std::move(std::move(func).get());

                    // TODO:LLVM use fake host triple,use true host triple to initialize JITTargetMachineBuilder
                    // TODO:optimize like clang -O3
                    auto JTMB = getLLVMResult(*ctx, llvm::orc::JITTargetMachineBuilder::detectHost());

                    // TODO:test settings
                    // TODO:FP Precise/Atomic
                    JTMB.setCodeGenOptLevel(llvm::CodeGenOpt::Aggressive);
                    JTMB.setRelocationModel(llvm::Reloc::Static);
                    JTMB.setCodeModel(llvm::CodeModel::Small);

                    llvm::TargetOptions& options = JTMB.getOptions();
                    options.EmulatedTLS = false;
                    options.TLSSize = 0;

                    // DEBUG
                    // options.PrintMachineCode = true;

                    /*
                    options.NoInfsFPMath = true;
                    options.NoNaNsFPMath = true;
                    options.NoSignedZerosFPMath = true;
                    options.NoTrapAfterNoreturn = true;
                    options.NoTrappingFPMath = true;
                    options.NoZerosInBSS = true;
                    options.FPDenormalMode = llvm::FPDenormal::PositiveZero;
                    options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
                    options.DebuggerTuning = llvm::DebuggerKind::Default;
                    options.CompressDebugSections = llvm::DebugCompressionType::None;
                    options.DataSections = false;
                    options.DisableIntegratedAS = false;
                    options.EABIVersion = llvm::EABI::EABI5;
                    options.EmitAddrsig = false;
                    options.EmitStackSizeSection = false;
                    options.EnableDebugEntryValues = false;
                    options.EnableFastISel = false;
                    options.EnableGlobalISel = true;
                    options.EnableIPRA = true;
                    options.EnableMachineOutliner = true;
                    options.ExceptionModel = llvm::ExceptionHandling::None;
                    options.ExplicitEmulatedTLS = false;
                    options.FloatABIType = llvm::FloatABI::Hard;
                    options.ForceDwarfFrameSection = false;
                    options.FunctionSections = false;
                    options.GlobalISelAbort = llvm::GlobalISelAbortMode::Disable;
                    options.HonorSignDependentRoundingFPMathOption = false;
                    options.GuaranteedTailCallOpt = true;
                    options.PrintMachineCode = false;
                    options.RelaxELFRelocations = false;
                    options.StackAlignmentOverride = false;
                    options.StackSymbolOrdering = true;
                    options.SupportsDefaultOutlining = true;
                    options.ThreadModel = llvm::ThreadModel::Single;
                    options.UnsafeFPMath = false;
                    */

                    auto engine = getLLVMResult(*ctx,
                                                std::move(llvm::orc::LLJITBuilder{}
                                                              .setNumCompileThreads(std::thread::hardware_concurrency())
                                                              .setJITTargetMachineBuilder(std::move(JTMB)))
                                                    .create());

                    mod->setDataLayout(engine->getDataLayout());
                    auto err = engine->addIRModule(llvm::orc::ThreadSafeModule{
                        std::move(mod), std::move(const_cast<std::remove_const_t<decltype(llvmctx)>&>(llvmctx)) });

                    if(err)
                        ctx->getErrorHandler().raiseException(llvm::toString(std::move(err)).c_str(), PIPER_SOURCE_LOCATION());

                    return eastl::static_shared_pointer_cast<RunnableProgram>(
                        makeSharedObject<LLVMProgram>(*ctx, std::move(engine), entry));
                },
                std::move(kernel));
        }
        void runKernel(uint32_t n, const Future<SharedPtr<RunnableProgram>>& kernel, const SharedPtr<Argument>& args) override {
            // TODO:for small n,run in the thread
            auto& scheduler = context().getScheduler();
            auto argsImpl = dynamic_cast<ArgumentImpl*>(args.get());
            auto&& binding = argsImpl->getResourceBinding();
            // TODO:reduce copy
            auto inputFuture = binding->getInput();
            Vector<Future<void>> input{ context().getAllocator() };
            input.reserve(inputFuture.size());
            for(auto&& in : inputFuture)
                input.push_back(Future<void>{ in });
            auto future =
                scheduler
                    .parallelFor((n + chunkSize - 1) / chunkSize,
                                 [arg = argsImpl->getArgument(), n](uint32_t idx, const Future<SharedPtr<RunnableProgram>>& func,
                                                                    const Future<void>&) {
                                     // TODO:unchecked cast
                                     auto kernel = dynamic_cast<LLVMProgram*>(func.get().get());
                                     uint32_t beg = idx * chunkSize;
                                     const uint32_t end = beg + chunkSize;
                                     if(end < n)
                                         kernel->runUnroll(beg, arg.data());
                                     else {
                                         while(beg < n) {
                                             kernel->run(beg, arg.data());
                                             ++beg;
                                         }
                                     }
                                 },
                                 kernel, scheduler.wrap(input))
                    .raw();
            binding->makeDirty(future);
        }
        void apply(Function<void, Context, CommandQueue> func, const SharedPtr<ResourceBinding>& binding) override {
            auto bind = dynamic_cast<ResourceBindingImpl*>(binding.get());
            auto input = bind->getInput();
            auto& scheduler = context().getScheduler();
            auto result = scheduler.newFutureImpl(0, false);
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [call = std::move(func)] { call(static_cast<Context>(0), currentThreadID()); } },
                                Span<const SharedPtr<FutureImpl>>{ input.data(), input.size() }, result);
            bind->makeDirty(result);
        }
        Future<void> available(const SharedPtr<Resource>& resource) override {
            auto res = dynamic_cast<ResourceImpl*>(resource.get());
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            return Future<void>{ res->getFuture() };
        }
        SharedPtr<Buffer> createBuffer(size_t size, size_t alignment) override {
            auto& allocator = context().getAllocator();
            Ptr data = allocator.alloc(size, alignment);
            return eastl::static_shared_pointer_cast<Buffer>(
                makeSharedObject<BufferImpl>(context(), data, size, allocator, makeSharedObject<ResourceImpl>(context(), data)));
        }
    };  // namespace Piper
    class ModuleImpl final : public Module {
    public:
        explicit ModuleImpl(PiperContext& context) : Module(context) {
            if(!llvm::llvm_is_multithreaded())
                throw;
            // TODO:reduce unused initializing
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Accelerator") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ParallelAccelerator>(context())));
            }
            throw;
        }
        ~ModuleImpl() {
            llvm::llvm_shutdown();
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
