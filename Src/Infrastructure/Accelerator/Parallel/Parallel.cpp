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
#include <Windows.h>
#include <new>
#pragma warning(push, 0)
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
// TODO:choose JIT
// to link OrcJIT
//#include <llvm/ExecutionEngine/OrcMCJITReplacement.h>
// to link MCJIT
#include <llvm/ExecutionEngine/MCJIT.h>
#pragma warning(pop)

namespace Piper {

    static CommandQueue currentThreadID() {
        return GetCurrentThreadId();
    }

    // TODO:set LLVM Allocator
    template <typename T>
    auto getLLVMResult(PiperContext& context, llvm::Expected<T> value) {
        if(value)
            return value.get();
        context.getErrorHandler().raiseException(("LLVM error " + llvm::toString(value.takeError())).c_str(),
                                                 PIPER_SOURCE_LOCATION());
    }

    // TODO:use stack parameter?
    class LLVMProgram final : public RunnableProgram {
    private:
        std::unique_ptr<llvm::ExecutionEngine> mExecutionContext;
        using KernelFunction = void (*)(uint32_t idx, const std::byte* payload);
        // TODO:ownership?
        KernelFunction mFunction;

    public:
        LLVMProgram(PiperContext& context, llvm::ExecutionEngine* executionContext, const String& entry)
            : RunnableProgram(context), mExecutionContext(executionContext),
              mFunction(reinterpret_cast<KernelFunction>(
                  executionContext->getFunctionAddress(llvm::StringRef{ entry.data(), entry.size() }))) {
            if(!mFunction)
                context.getErrorHandler().raiseException("Undefined entry " + entry, PIPER_SOURCE_LOCATION());
        }
        void run(const uint32_t idx, const std::byte* payload) {
            mFunction(idx, payload);
        }
    };

    class ResourceImpl final : public Resource {
    private:
        SharedObject<FutureImpl> mFuture;

    public:
        ResourceImpl(PiperContext& context, const ResourceHandle& handle) : Resource(context, handle) {}
        SharedObject<FutureImpl> getFuture() const {
            return mFuture;
        }
        void setFuture(const SharedObject<FutureImpl>& future) {
            // TODO:formal check
            if(mFuture.unique() && !mFuture->ready())
                throw;
            mFuture = future;
        }
    };

    class ResourceBindingImpl final : public ResourceBinding {
    private:
        Vector<SharedObject<ResourceImpl>> mInput;
        Vector<SharedObject<ResourceImpl>> mOutput;

    public:
        explicit ResourceBindingImpl(PiperContext& context)
            : ResourceBinding(context), mInput(context.getAllocator()), mOutput(context.getAllocator()) {}
        void addInput(const SharedObject<Resource>& resource) override {
            auto res = eastl::dynamic_pointer_cast<ResourceImpl>(resource);
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            mInput.push_back(res);
        }
        void addOutput(const SharedObject<Resource>& resource) override {
            auto res = eastl::dynamic_pointer_cast<ResourceImpl>(resource);
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            mOutput.push_back(res);
        }
        Vector<SharedObject<FutureImpl>> getInput() const {
            Vector<SharedObject<FutureImpl>> input{ context().getAllocator() };
            input.reserve(mInput.size());
            for(auto&& in : mInput)
                input.push_back(in->getFuture());
            return input;
        }
        void makeDirty(const SharedObject<FutureImpl>& newFuture) {
            for(auto&& output : mOutput)
                output->setFuture(newFuture);
        }
    };

    class ParameterImpl final : public Parameter {
    private:
        Vector<std::byte> mParameter;
        SharedObject<ResourceBindingImpl> mResourceBinding;

    public:
        explicit ParameterImpl(PiperContext& context)
            : Parameter(context), mParameter(STLAllocator{ context.getAllocator() }),
              mResourceBinding(makeSharedObject<ResourceBindingImpl>(context)) {}
        void appendInput(const SharedObject<Resource>& resource) override {
            addExtraInput(resource);
            Parameter::append(resource->getHandle());
        }
        void appendOutput(const SharedObject<Resource>& resource) override {
            addExtraOutput(resource);
            Parameter::append(resource->getHandle());
        }
        void append(const void* data, const size_t size, const size_t alignment) override {
            // TODO:warning for padding
            auto rem = mParameter.size() % alignment;
            if(rem)
                mParameter.insert(mParameter.cend(), alignment - rem, std::byte{ 0 });
            auto beg = reinterpret_cast<const std::byte*>(data), end = beg + size;
            mParameter.insert(mParameter.cend(), beg, end);
        }
        void appendAccumulate(const SharedObject<Resource>& resource) {
            addExtraInput(resource);
            addExtraOutput(resource);
            Parameter::append(resource->getHandle());
        }
        void addExtraInput(const SharedObject<Resource>& resource) override {
            mResourceBinding->addInput(resource);
        }
        void addExtraOutput(const SharedObject<Resource>& resource) override {
            mResourceBinding->addOutput(resource);
        }
        Vector<std::byte> getParameter() const {
            return mParameter;
        }
        SharedObject<ResourceBindingImpl> getResourceBinding() const {
            return mResourceBinding;
        }
    };

    class BufferImpl final : public Buffer {
    private:
        Ptr mData;
        size_t mSize;
        Allocator& mAllocator;
        SharedObject<ResourceImpl> mResource;

    public:
        BufferImpl(PiperContext& context, Ptr data, const size_t size, Allocator& allocator,
                   const SharedObject<ResourceImpl>& res)
            : Buffer(context), mData(data), mSize(size), mAllocator(allocator), mResource(res) {}
        size_t size() const noexcept override {
            return mSize;
        }
        void upload(const void* data) override {
            auto& scheduler = context().getScheduler();
            auto res = scheduler.newFutureImpl(0, false);
            auto dep = mResource->getFuture();
            auto depList = std::initializer_list<const SharedObject<FutureImpl>>{ dep };
            scheduler.spawnImpl(
                Closure<>{ context(), context().getAllocator(),
                           [dest = mData, src = data, size = mSize] { memcpy(reinterpret_cast<void*>(dest), src, size); } },
                Span<const SharedObject<FutureImpl>>{ depList.begin(), depList.end() }, res);
            mResource->setFuture(res);
        }
        Future<Vector<std::byte>> download() const override {
            auto& scheduler = context().getScheduler();
            // TODO:ownership?
            return scheduler.spawn(
                [this](const Future<void>&) {
                    auto beg = reinterpret_cast<const std::byte*>(mData), end = beg + mSize;
                    return Vector<std::byte>{ beg, end, context().getAllocator() };
                },
                Future<void>{ mResource->getFuture() });
        }
        void reset() override {
            auto& scheduler = context().getScheduler();
            auto res = scheduler.newFutureImpl(0, false);
            auto dep = mResource->getFuture();
            auto depList = std::initializer_list<const SharedObject<FutureImpl>>{ dep };
            scheduler.spawnImpl(Closure<>{ context(), context().getAllocator(),
                                           [dest = mData, size = mSize] { memset(reinterpret_cast<void*>(dest), 0x00, size); } },
                                Span<const SharedObject<FutureImpl>>{ depList.begin(), depList.end() }, res);
            mResource->setFuture(res);
        }
        SharedObject<Resource> ref() const override {
            return eastl::static_shared_pointer_cast<Resource>(mResource);
        }
    };

    class ParallelAccelerator final : public Accelerator {
    private:
        CString mSupportedLinkable;
        llvm::LLVMContext mContext;

    public:
        // TODO:support FFI,DLL
        explicit ParallelAccelerator(PiperContext& context) : Accelerator(context), mSupportedLinkable("LLVM IR") {}
        Span<const CString> getSupportedLinkableFormat() const override {
            return Span<const CString>{ &mSupportedLinkable, 1 };
        }
        SharedObject<ResourceBinding> createResourceBinding() const override {
            return eastl::static_shared_pointer_cast<ResourceBinding>(makeSharedObject<ResourceBindingImpl>(context()));
        }
        SharedObject<Parameter> createParameters() const override {
            return eastl::static_shared_pointer_cast<Parameter>(makeSharedObject<ParameterImpl>(context()));
        }
        SharedObject<Resource> createResource(const ResourceHandle handle) const override {
            return eastl::static_shared_pointer_cast<Resource>(makeSharedObject<ResourceImpl>(context(), handle));
        }
        // TODO:parallel unroll 16
        Future<SharedObject<RunnableProgram>> compileKernel(const Vector<Future<Vector<std::byte>>>& linkable,
                                                            const String& entry) override {
            Vector<Future<std::unique_ptr<llvm::Module>>> modules{ context().getAllocator() };
            modules.reserve(linkable.size());
            auto& scheduler = context().getScheduler();
            // TODO:use parallel_for?
            for(auto&& unit : linkable)
                // TODO:Is llvm::LLVMContext thread-safe?
                modules.emplace_back(std::move(scheduler.spawn(
                    [ctx = &context(), llvmctx = &mContext](const Future<Vector<std::byte>>& data) {
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
                [ctx = &context(), llvmctx = &mContext](const Future<Vector<std::unique_ptr<llvm::Module>>>& mods) {
                    auto stage = ctx->getErrorHandler().enterStageStatic("Link LLVM Modules", PIPER_SOURCE_LOCATION());
                    auto& units = mods.get();
                    auto kernel = std::make_unique<llvm::Module>("LLVM kernel", *llvmctx);
                    for(auto&& unit : units)
                        llvm::Linker::linkModules(*kernel, llvm::CloneModule(*unit));
                    return std::move(kernel);
                },
                scheduler.wrap(modules));
            return scheduler.spawn(
                [entry, ctx = &context()](Future<std::unique_ptr<llvm::Module>> func) {
                    auto mod = std::move(std::move(func).get());
                    std::string error;
                    // TODO:host CPU
                    auto engine = llvm::EngineBuilder{ std::move(mod) }
                                      .setEngineKind(llvm::EngineKind::JIT)
                                      .setErrorStr(&error)
                                      .setOptLevel(llvm::CodeGenOpt::Aggressive)
                                      .create();
                    //TODO:JIT is locked
                    if(engine) {
                        engine->DisableLazyCompilation();
                        engine->setProcessAllSections(true);
                        engine->finalizeObject();
                        return eastl::static_shared_pointer_cast<RunnableProgram>(
                            makeSharedObject<LLVMProgram>(*ctx, engine, entry));
                    }
                    ctx->getErrorHandler().raiseException(StringView{ error.c_str(), error.size() }, PIPER_SOURCE_LOCATION());
                },
                std::move(kernel));
        }
        void runKernel(uint32_t n, const Future<SharedObject<RunnableProgram>>& kernel,
                       const SharedObject<Parameter>& params) override {
            auto& scheduler = context().getScheduler();
            auto paramsImpl = dynamic_cast<ParameterImpl*>(params.get());
            auto&& binding = paramsImpl->getResourceBinding();
            // TODO:reduce copy
            auto inputFuture = binding->getInput();
            Vector<Future<void>> input{ context().getAllocator() };
            input.reserve(inputFuture.size());
            for(auto&& in : inputFuture)
                input.push_back(Future<void>{ in });
            constexpr auto chunkSize = 64;
            auto future =
                scheduler
                    .parallelFor((n + chunkSize - 1) / chunkSize,
                                 [param = paramsImpl->getParameter(), n, chunkSize](
                                     uint32_t idx, const Future<SharedObject<RunnableProgram>>& func, const Future<void>&) {
                                     auto kernel = dynamic_cast<LLVMProgram*>(func.get().get());
                                     uint32_t beg = idx * chunkSize, end = std::min(beg + chunkSize, n);
                                     while(beg < end) {
                                         kernel->run(beg, param.data());
                                         ++beg;
                                     }
                                 },
                                 kernel, scheduler.wrap(input))
                    .raw();
            binding->makeDirty(future);
        }
        void apply(Function<void, Context, CommandQueue> func, const SharedObject<ResourceBinding>& binding) override {
            auto bind = dynamic_cast<ResourceBindingImpl*>(binding.get());
            auto input = bind->getInput();
            auto& scheduler = context().getScheduler();
            auto result = scheduler.newFutureImpl(0, false);
            scheduler.spawnImpl(
                Closure{ context(), context().getAllocator(), [call = std::move(func)] { call(0, currentThreadID()); } },
                Span<const SharedObject<FutureImpl>>{ input.data(), input.size() }, result);
            bind->makeDirty(result);
        }
        Future<void> available(const SharedObject<Resource>& resource) override {
            auto res = dynamic_cast<ResourceImpl*>(resource.get());
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            return Future<void>{ res->getFuture() };
        }
        SharedObject<Buffer> createBuffer(size_t size, size_t alignment) override {
            auto& allocator = context().getAllocator();
            Ptr data = allocator.alloc(size, alignment);
            return eastl::static_shared_pointer_cast<Buffer>(
                makeSharedObject<BufferImpl>(context(), data, size, allocator, makeSharedObject<ResourceImpl>(context(), data)));
        }
    };
    class ModuleImpl final : public Module {
    public:
        explicit ModuleImpl(PiperContext& context) : Module(context) {
            // TODO:reduce unused initializing
            llvm::InitializeNativeTarget();
            // llvm::InitializeNativeTargetAsmParser();
            llvm::InitializeNativeTargetAsmPrinter();
            // llvm::InitializeNativeTargetDisassembler();
        }
        Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                 const Future<void>& module) override {
            if(classID == "Accelerator") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ParallelAccelerator>(context())));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
