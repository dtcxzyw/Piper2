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
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
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
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#pragma warning(pop)
#include <llvm/Linker/Linker.h>
#include <new>
#include <utility>

namespace Piper {

    class LLVMStream final : public llvm::raw_pwrite_stream {
    private:
        PiperContext& mContext;
        Binary& mData;

    public:
        explicit LLVMStream(PiperContext& context, Binary& data) : raw_pwrite_stream(true), mContext(context), mData(data) {}
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
        // NOTICE: return true if the module is broken
        if(llvm::verifyModule(module, &out)) {
            out.flush();
            context.getErrorHandler().raiseException(("Bad module:" + output).c_str(), PIPER_SOURCE_LOCATION());
        }
    }

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
    static Binary module2Binary(PiperContext& context, llvm::Module& module) {
        Binary data{ context.getAllocator() };
        LLVMStream stream(context, data);
        llvm::WriteBitcodeToFile(module, stream);
        stream.flush();
        return data;
    }

    class LLVMIR final : public PITU, public eastl::enable_shared_from_this<LLVMIR> {
    private:
        // TODO: lazy cast
        Binary mModule;
        uint64_t mUID;

    public:
        LLVMIR(PiperContext& context, Binary module, const uint64_t UID) : PITU(context), mModule(std::move(module)), mUID(UID) {}
        String humanReadable() const override {
            llvm::LLVMContext ctx;
            const auto inst = binary2Module(context(), mModule, ctx);
            std::string res;
            llvm::raw_string_ostream out(res);
            llvm::legacy::PassManager manager;
            manager.add(llvm::createPrintModulePass(out));
            manager.run(*inst);
            out.flush();
            return String{ res.data(), res.size(), context().getAllocator() };
        }
        uint64_t getID() const noexcept {
            return mUID;
        }
        const Binary& data() const noexcept {
            return mModule;
        }
        LinkableProgram generateLinkable(const Span<const CString>& acceptableFormat) const override {
            auto& scheduler = context().getScheduler();
            for(auto&& format : acceptableFormat) {
                // TODO: LLVM IR Version
                if(StringView{ format } == "LLVM IR") {
                    return LinkableProgram{ scheduler.value(eastl::dynamic_shared_pointer_cast<PITU>(shared_from_this())),
                                            String{ "LLVM IR", context().getAllocator() }, mUID };
                }
                if(StringView{ format } == "LLVM IR Bitcode") {
                    return LinkableProgram{ scheduler.value(mModule), String{ "LLVM IR Bitcode", context().getAllocator() },
                                            mUID };
                }
                std::string error;
                const auto* target = llvm::TargetRegistry::lookupTarget(format, error);
                if(target) {
                    return LinkableProgram{ scheduler.spawn([target, format, self = shared_from_this()] {
                                               auto stage = self->context().getErrorHandler().enterStage("generate linkable",
                                                                                                         PIPER_SOURCE_LOCATION());
                                               // TODO:llvm::sys::getHostCPUName();llvm::sys::getHostCPUFeatures();
                                               const auto* cpu = "generic";
                                               const auto* features = "";

                                               llvm::TargetOptions opt;
                                               auto RM = llvm::Optional<llvm::Reloc::Model>();
                                               auto* targetMachine = target->createTargetMachine(format, cpu, features, opt, RM);

                                               llvm::LLVMContext ctx;
                                               const auto inst = binary2Module(self->context(), self->mModule, ctx);

                                               inst->setDataLayout(targetMachine->createDataLayout());
                                               inst->setTargetTriple(format);

                                               llvm::legacy::PassManager pass;
                                               Binary data{ self->context().getAllocator() };
                                               LLVMStream stream(self->context(), data);
                                               if(!(targetMachine->addPassesToEmitFile(pass, stream, nullptr,
                                                                                       llvm::CodeGenFileType::CGFT_ObjectFile)))
                                                   self->context().getErrorHandler().raiseException(
                                                       "Failed to create object emit pass", PIPER_SOURCE_LOCATION());
                                               pass.run(*inst);
                                               // TODO: optimize
                                               stream.flush();
                                               return std::move(data);
                                           }),
                                            String{ format, context().getAllocator() }, mUID };
                }
            }
            std::string out;
            llvm::raw_string_ostream output(out);
            llvm::TargetRegistry::printRegisteredTargetsForVersion(output);
            output.flush();
            context().getErrorHandler().raiseException(
                ("No supported linkable program format. Only target " + out + " are supported.").c_str(),
                PIPER_SOURCE_LOCATION());
        }
    };

    class LLVMIRManager final : public PITUManager {
    public:
        explicit LLVMIRManager(PiperContext& context) : PITUManager(context) {}
        [[nodiscard]] Future<SharedPtr<PITU>> loadPITU(const String& path) const override {
            return context().getScheduler().spawn([ctx = &context(), path] {
                auto stage = ctx->getErrorHandler().enterStage("load PITU " + path, PIPER_SOURCE_LOCATION());
                const auto file = ctx->getFileSystem().mapFile(path, FileAccessMode::Read, FileCacheHint::Sequential);
                const auto map = file->map(0, file->size());
                const auto span = map->get();
                return eastl::static_shared_pointer_cast<PITU>(makeSharedObject<LLVMIR>(
                    *ctx, Binary{ span.begin(), span.end(), ctx->getAllocator() }, eastl::hash<String>{}(path)));
            });
        }

        [[nodiscard]] Future<SharedPtr<PITU>> linkPITU(const DynamicArray<Future<SharedPtr<PITU>>>& pitus,
                                                       UMap<String, String> staticRedirectedSymbols,
                                                       DynamicArray<String> dynamicSymbols) const override {
            auto&& scheduler = context().getScheduler();

            return scheduler.spawn(
                [ctx = &context(), SRS = std::move(staticRedirectedSymbols),
                 DST = std::move(dynamicSymbols)](DynamicArray<SharedPtr<PITU>> modules) {
                    auto&& errorHandler = ctx->getErrorHandler();
                    auto stage = errorHandler.enterStage("link LLVM modules", PIPER_SOURCE_LOCATION());

                    // TODO: use custom allocator
                    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
                    uint64_t UID = 0;

                    auto module = std::make_unique<llvm::Module>("linkedPITU", *llvmCtx);
                    llvm::Linker linker{ *module };

                    for(auto& mod : modules) {
                        const auto* ir = dynamic_cast<const LLVMIR*>(mod.get());
                        if(!ir)
                            ctx->getErrorHandler().raiseException("Unsupported PITU", PIPER_SOURCE_LOCATION());
                        linker.linkInModule(binary2Module(*ctx, ir->data(), *llvmCtx));
                        UID ^= ir->getID();
                    }

                    stage.next("construct builtin symbol redirect", PIPER_SOURCE_LOCATION());

                    auto findSymbol = [&](const String& symbol) -> llvm::Constant* {
                        const llvm::StringRef name{ symbol.data(), symbol.size() };
                        if(const auto func = module->getFunction(name))
                            return func;
                        if(const auto var = module->getNamedGlobal(name))
                            return var;
                        if(const auto alias = module->getNamedAlias(name))
                            return alias;

                        errorHandler.raiseException("Undefined symbol \"" + symbol + '\"', PIPER_SOURCE_LOCATION());
                    };

                    for(auto&& [dst, src] : SRS) {
                        const auto symbol = findSymbol(src);

                        if(symbol->getType()->isFunctionTy())
                            llvm::GlobalIFunc::create(symbol->getType(), 0, llvm::GlobalValue::LinkageTypes::ExternalLinkage,
                                                      llvm::StringRef{ dst.data(), dst.size() }, symbol, module.get());
                        else
                            llvm::GlobalAlias::create(symbol->getType(), 0, llvm::GlobalValue::LinkageTypes::ExternalLinkage,
                                                      llvm::StringRef{ dst.data(), dst.size() }, symbol, module.get());
                    }

                    const auto pointer = llvm::Type::getVoidTy(*llvmCtx)->getPointerTo();
                    const auto arrayType = llvm::ArrayType::get(pointer, DST.size());
                    DynamicArray<llvm::Constant*> symbols{ DST.size(), ctx->getAllocator() };
                    std::transform(DST.cbegin(), DST.cend(), symbols.begin(),
                                   [&](const String& name) { return findSymbol(name); });
                    llvm::Constant* values = llvm::ConstantArray::get(
                        arrayType, llvm::ArrayRef<llvm::Constant*>{ symbols.data(), symbols.data() + symbols.size() });

                    // TODO: Is it memory safe?
                    module->getOrInsertGlobal("piperBuiltinSymbolLUT", arrayType, [&] {
                        return new llvm::GlobalVariable{ arrayType, true, llvm::GlobalValue::LinkageTypes::ExternalLinkage,
                                                         values, "piperBuiltinSymbolLUT" };
                    });

                    // TODO: verification switch
                    verifyLLVMModule(*ctx, *module);

                    return eastl::static_shared_pointer_cast<PITU>(
                        makeSharedObject<LLVMIR>(*ctx, module2Binary(*ctx, *module), UID));
                },
                scheduler.wrap(pitus));
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
