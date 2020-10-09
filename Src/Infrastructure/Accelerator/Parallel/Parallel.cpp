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
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Transforms/Utils/Cloning.h>
// to link OrcJIT
//#include <llvm/ExecutionEngine/OrcMCJITReplacement.h>
// to link Interpreter
#include <llvm/ExecutionEngine/Interpreter.h>
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

    class LLVMProgram final : public RunnableProgram {
    private:
        std::unique_ptr<llvm::ExecutionEngine> mExecutionContext;
        // TODO:ownership?
        llvm::Function* mFunction;

    public:
        LLVMProgram(PiperContext& context, llvm::ExecutionEngine* executionContext, const String& entry)
            : RunnableProgram(context), mExecutionContext(executionContext),
              mFunction(executionContext->FindFunctionNamed(llvm::StringRef{ entry.data(), entry.size() })) {
            if(!mFunction)
                context.getErrorHandler().raiseException("Undefined entry " + entry, PIPER_SOURCE_LOCATION());
        }
        void run(const llvm::ArrayRef<llvm::GenericValue>& parameters) {
            mExecutionContext->runFunction(mFunction, parameters);
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
            mFuture = future;
        }
    };

    class ResourceBindingImpl final : public ResourceBinding {
    private:
        Vector<SharedObject<FutureImpl>> mInput;
        Vector<SharedObject<ResourceImpl>> mOutput;

    public:
        explicit ResourceBindingImpl(PiperContext& context)
            : ResourceBinding(context), mInput(context.getAllocator()), mOutput(context.getAllocator()) {}
        void addInput(const SharedObject<Resource>& resource) override {
            auto future = eastl::dynamic_pointer_cast<ResourceImpl>(resource)->getFuture();
            mInput.push_back();
        }
        void addOutput(const SharedObject<Resource>& resource) override {
            auto res = eastl::dynamic_pointer_cast<ResourceImpl>(resource);
            if(!res)
                context().getErrorHandler().raiseException("Unrecognized Resource", PIPER_SOURCE_LOCATION());
            mOutput.push_back(res);
        }
        const Vector<SharedObject<FutureImpl>>& getInput() const {
            return mInput;
        }
        void makeDirty(const SharedObject<FutureImpl>& newFuture) {
            for(auto&& output : mOutput)
                output->setFuture(newFuture);
        }
    };

    class ParameterImpl final : public Parameter {
    private:
        Vector<Vector<std::byte>> mStructData;
        Vector<llvm::GenericValue> mParameter;
        SharedObject<ResourceBindingImpl> mResourceBinding;

        llvm::GenericValue& locate(uint32_t idx) {
            ++idx;  // for index parameter
            if(idx >= mParameter.size())
                mParameter.resize(idx + 1);
            return mParameter[idx];
        }

    public:
        explicit ParameterImpl(PiperContext& context)
            : Parameter(context), mStructData(context.getAllocator()), mParameter(1, context.getAllocator()),
              mResourceBinding(makeSharedObject<ResourceBindingImpl>(context)) {}
        void bindInput(uint32_t slot, const SharedObject<Resource>& resource) override {
            addExtraInput(resource);
            bindUInt(slot, resource->getHandle(), 64);
        }
        void bindOutput(uint32_t slot, const SharedObject<Resource>& resource) override {
            addExtraOutput(resource);
            bindUInt(slot, resource->getHandle(), 64);
        }
        void bindStructure(uint32_t slot, const void* data, const size_t size) override {
            auto beg = reinterpret_cast<const std::byte*>(data), end = beg + size;
            mStructData.push_back({ beg, end, context().getAllocator() });
            locate(slot) = llvm::PTOGV(mStructData.back().data());
        }
        void bindFloat32(const uint32_t slot, const float value) override {
            llvm::GenericValue val;
            val.FloatVal = value;
            locate(slot) = val;
        }
        void bindFloat64(const uint32_t slot, const double value) override {
            llvm::GenericValue val;
            val.DoubleVal = value;
            locate(slot) = val;
        }
        void bindInt(const uint32_t slot, const intmax_t value, const uint32_t bits) override {
            llvm::GenericValue val;
            val.IntVal = llvm::APInt(bits, static_cast<uint64_t>(value), true);
            locate(slot) = val;
        }
        void bindUInt(const uint32_t slot, const uintmax_t value, const uint32_t bits) override {
            llvm::GenericValue val;
            val.IntVal = llvm::APInt(bits, value);
            locate(slot) = val;
        }
        void addExtraInput(const SharedObject<Resource>& resource) override {
            mResourceBinding->addInput(resource);
        }
        void addExtraOutput(const SharedObject<Resource>& resource) override {
            mResourceBinding->addOutput(resource);
        }
        Vector<llvm::GenericValue> getParameter() const {
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
            auto dep = std::initializer_list<const SharedObject<FutureImpl>>{ mResource->getFuture() };
            scheduler.spawnImpl(
                Closure<>{ context().getAllocator(),
                           [dest = mData, src = data, size = mSize] { memcpy(reinterpret_cast<void*>(dest), src, size); } },
                Span<const SharedObject<FutureImpl>>{ dep.begin(), dep.end() }, res);
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
            auto dep = std::initializer_list<const SharedObject<FutureImpl>>{ mResource->getFuture() };
            scheduler.spawnImpl(Closure<>{ context().getAllocator(),
                                           [dest = mData, size = mSize] { memset(reinterpret_cast<void*>(dest), 0x00, size); } },
                                Span<const SharedObject<FutureImpl>>{ dep.begin(), dep.end() }, res);
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
                    auto engine = llvm::EngineBuilder{ std::move(mod) }
                                      .setEngineKind(llvm::EngineKind::Interpreter)
                                      .setErrorStr(&error)
                                      .setOptLevel(llvm::CodeGenOpt::Aggressive)
                                      .create();
                    if(engine)
                        return eastl::static_shared_pointer_cast<RunnableProgram>(
                            makeSharedObject<LLVMProgram>(*ctx, engine, entry));
                    ctx->getErrorHandler().raiseException(StringView{ error.c_str(), error.size() }, PIPER_SOURCE_LOCATION());
                },
                std::move(kernel));
        }
        void runKernel(uint32_t n, const Future<SharedObject<RunnableProgram>>& kernel,
                       const SharedObject<Parameter>& params) override {
            auto& scheduler = context().getScheduler();
            auto paramsImpl = dynamic_cast<ParameterImpl*>(params.get());
            auto&& binding = paramsImpl->getResourceBinding();
            auto future = scheduler
                              .parallelFor(
                                  n,
                                  [params, paramsImpl](uint32_t idx, const Future<SharedObject<RunnableProgram>>& func) {
                                      // TODO:reduce parameter copy using chunk
                                      auto kernel = dynamic_cast<LLVMProgram*>(func.get().get());
                                      auto parameter = paramsImpl->getParameter();
                                      parameter[0].IntVal = llvm::APInt(32, idx);
                                      kernel->run(llvm::ArrayRef<llvm::GenericValue>{ parameter.data(), parameter.size() });
                                  },
                                  kernel)
                              .raw();
            binding->makeDirty(future);
        }
        void apply(Function<void, Context, CommandQueue> func, const SharedObject<ResourceBinding>& binding) override {
            auto bind = dynamic_cast<ResourceBindingImpl*>(binding.get());
            auto& input = bind->getInput();
            auto& scheduler = context().getScheduler();
            auto result = scheduler.newFutureImpl(0, false);
            scheduler.spawnImpl(Closure{ context().getAllocator(), [call = std::move(func)] { call(0, currentThreadID()); } },
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
