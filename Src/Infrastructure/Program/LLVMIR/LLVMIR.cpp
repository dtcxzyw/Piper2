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
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/PerformancePrimitivesLibrary.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include "../../../STL/UniquePtr.hpp"
#pragma warning(push, 0)
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
// TODO:use new PassManager
//#include <llvm/IR/PassManager.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Utils/Cloning.h>
#pragma warning(pop)
#include <mutex>
#include <new>
#include <utility>

namespace Piper {
    struct Context final {
        llvm::LLVMContext context;
        std::mutex mutex;
    };

    class LLVMStream final : public llvm::raw_pwrite_stream {
    private:
        PiperContext& mContext;
        DynamicArray<std::byte>& mData;

    public:
        explicit LLVMStream(PiperContext& context, DynamicArray<std::byte>& data)
            : raw_pwrite_stream(true), mContext(context), mData(data) {}
        void pwrite_impl(const char* ptr, const size_t size, const uint64_t offset) override {
            if(offset != mData.size())
                mContext.getErrorHandler().assertFailed(ErrorHandler::CheckLevel::InternalInvariant,
                                                        "Bitcode buffer internal error occurred.", PIPER_SOURCE_LOCATION());
            write_impl(ptr, size);
        }
        void write_impl(const char* ptr, const size_t size) override {
            const auto* beg = reinterpret_cast<const std::byte*>(ptr);
            const auto* end = beg + size;
            mData.insert(mData.cend(), beg, end);
        }

        [[nodiscard]] uint64_t current_pos() const override {
            return mData.size();
        }
    };

    static void verifyLLVMModule(PiperContext& context, llvm::Module& module) {
        std::string output;
        llvm::raw_string_ostream out(output);
        // ReSharper disable once CppRedundantQualifier
        if(llvm::verifyModule(module, &out)) {
            out.flush();
            context.getErrorHandler().raiseException(("Bad module:" + output).c_str(), PIPER_SOURCE_LOCATION());
        }
    }

    class LLVMIR final : public PITU, public eastl::enable_shared_from_this<LLVMIR> {
    private:
        mutable SharedPtr<Context> mContext;
        mutable std::unique_ptr<llvm::Module> mModule;
        mutable std::mutex mMutex;

    public:
        LLVMIR(PiperContext& context, SharedPtr<Context> llvmCtx, std::unique_ptr<llvm::Module> module)
            : PITU(context), mContext(std::move(llvmCtx)), mModule(std::move(module)) {}
        // TODO:reduce clone
        std::unique_ptr<llvm::Module> cloneModule() const {
            std::lock_guard<std::mutex> guard{ mContext->mutex };
            // ReSharper disable once CppRedundantQualifier
            return llvm::CloneModule(*mModule);
        }
        String humanReadable() const override {
            std::lock_guard<std::mutex> guard{ mMutex };
            std::string res;
            llvm::raw_string_ostream out(res);
            llvm::legacy::PassManager manager;
            manager.add(llvm::createPrintModulePass(out));
            manager.run(*mModule);
            out.flush();
            return String{ res.data(), res.size(), context().getAllocator() };
        }
        Future<LinkableProgram> generateLinkable(const Span<const CString>& acceptableFormat) const override {
            auto& scheduler = context().getScheduler();
            for(auto&& format : acceptableFormat) {
                // TODO:LLVM IR Version
                if(StringView{ format } == "LLVM IR") {
                    // TODO:lazy parse linkable and directly return data
                    return scheduler.spawn(
                        [ctx = &context()](const std::unique_ptr<llvm::Module> mod) {
                            DynamicArray<std::byte> data{ ctx->getAllocator() };
                            LLVMStream stream(*ctx, data);
                            llvm::WriteBitcodeToFile(*mod, stream);
                            stream.flush();
                            return LinkableProgram{ std::move(data), "LLVM IR" };
                        },
                        scheduler.value(cloneModule()));
                }
                std::string error;
                const auto* target = llvm::TargetRegistry::lookupTarget(format, error);
                if(target) {
                    return scheduler.spawn(
                        [target, format, ctx = &context()](std::unique_ptr<llvm::Module> mod) {
                            auto stage = ctx->getErrorHandler().enterStage("generate linkable", PIPER_SOURCE_LOCATION());
                            // TODO:llvm::sys::getHostCPUName();llvm::sys::getHostCPUFeatures();
                            const auto* cpu = "generic";
                            const auto* features = "";

                            llvm::TargetOptions opt;
                            auto RM = llvm::Optional<llvm::Reloc::Model>();
                            auto* targetMachine = target->createTargetMachine(format, cpu, features, opt, RM);
                            // BUG:The operation is not thread safe.
                            // mod->setDataLayout(targetMachine->createDataLayout());
                            // mod->setTargetTriple(format);

                            llvm::legacy::PassManager pass;
                            DynamicArray<std::byte> data{ ctx->getAllocator() };
                            LLVMStream stream(*ctx, data);
                            if(!(targetMachine->addPassesToEmitFile(pass, stream, nullptr,
                                                                    llvm::CodeGenFileType::CGFT_ObjectFile)))
                                ctx->getErrorHandler().raiseException("Failed to create object emit pass",
                                                                      PIPER_SOURCE_LOCATION());
                            pass.run(*mod);
                            stream.flush();
                            return LinkableProgram{ std::move(data), format };
                        },
                        scheduler.value(cloneModule()));
                }
            }
            std::string out;
            llvm::raw_string_ostream output(out);
            llvm::TargetRegistry::printRegisteredTargetsForVersion(output);
            output.flush();
            context().getErrorHandler().raiseException(
                ("No supported linkable program format.Only target " + out + " are supported.").c_str(), PIPER_SOURCE_LOCATION());
        }
    };

    class LLVMIRManager final : public PITUManager {
    private:
        SharedPtr<Context> mContext;

    public:
        explicit LLVMIRManager(PiperContext& context)
            : PITUManager(context), mContext(makeSharedPtr<Context>(context.getAllocator())) {}
        [[nodiscard]] Future<SharedPtr<PITU>> loadPITU(const String& path) const override {
            return context().getScheduler().spawn([ctx = &context(), path, llvmCtx = mContext] {
                auto stage = ctx->getErrorHandler().enterStage("load PITU " + path, PIPER_SOURCE_LOCATION());
                const auto file = ctx->getFileSystem().mapFile(path, FileAccessMode::Read, FileCacheHint::Sequential);
                const auto map = file->map(0, file->size());
                const auto span = map->get();
                const auto beg = reinterpret_cast<uint8_t*>(span.data());
                const auto end = beg + span.size();
                stage.next("parse LLVM bitcode", PIPER_SOURCE_LOCATION());

                std::unique_lock<std::mutex> guard{ llvmCtx->mutex };
                auto res = llvm::parseBitcodeFile(
                    llvm::MemoryBufferRef{ toStringRef(llvm::ArrayRef<uint8_t>{ beg, end }), "llvm bitcode data" },
                    llvmCtx->context);
                guard.unlock();

                if(res)
                    return eastl::static_shared_pointer_cast<PITU>(makeSharedObject<LLVMIR>(*ctx, llvmCtx, std::move(res.get())));

                const auto error = toString(res.takeError());
                ctx->getErrorHandler().raiseException(StringView{ error.c_str(), error.size() }, PIPER_SOURCE_LOCATION());
            });
        }
        [[nodiscard]] Future<SharedPtr<PITU>> mergePITU(const Future<DynamicArray<SharedPtr<PITU>>>& pitus) const override {
            return context().getScheduler().spawn(
                [ctx = &context(), llvmCtx = mContext](const DynamicArray<SharedPtr<PITU>>& modules) {
                    auto stage = ctx->getErrorHandler().enterStage("link LLVM modules", PIPER_SOURCE_LOCATION());

                    // TODO:module ID param
                    std::unique_lock<std::mutex> guard{ llvmCtx->mutex };
                    auto module = std::make_unique<llvm::Module>("merged_module", llvmCtx->context);
                    llvm::Linker linker(*module);

                    for(auto& mod : modules) {
                        const auto* ir = dynamic_cast<const LLVMIR*>(mod.get());
                        if(!ir)
                            ctx->getErrorHandler().raiseException("Unsupported PITU", PIPER_SOURCE_LOCATION());
                        linker.linkInModule(ir->cloneModule());
                    }
                    guard.unlock();

                    verifyLLVMModule(*ctx, *module);

                    return eastl::static_shared_pointer_cast<PITU>(makeSharedObject<LLVMIR>(*ctx, llvmCtx, std::move(module)));
                },
                pitus);
        }
    };
    class ModuleImpl final : public Module {
    public:
        explicit ModuleImpl(PiperContext& context, const char*) : Module(context) {
            if(!llvm::llvm_is_multithreaded())
                context.getErrorHandler().raiseException("LLVM should be compiled with multi-threading flag.",
                                                         PIPER_SOURCE_LOCATION());
            // llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            // llvm::InitializeAllTargetMCs();
            // llvm::InitializeAllAsmParsers();
            llvm::InitializeAllAsmPrinters();
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "LLVMIRManager") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<LLVMIRManager>(context())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
        ~ModuleImpl() noexcept override {
            llvm::llvm_shutdown();
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
