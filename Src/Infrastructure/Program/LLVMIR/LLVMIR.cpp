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
#include <llvm/IR/LLVMContext.h>
// TODO:use new PassManager
//#include <llvm/IR/PassManager.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Utils/Cloning.h>
#pragma warning(pop)
#include <llvm/Support/ManagedStatic.h>
#include <mutex>
#include <new>
#include <utility>

namespace Piper {
    class FPReplacer final : public llvm::ModulePass {
    private:
        // TODO:UMap
        std::unordered_map<String, std::pair<size_t, size_t>, eastl::hash<String>> mFPTypes;
        STLAllocator mAllocator;
        static char ID;

    public:
        explicit FPReplacer(const Vector<SharedPtr<FloatingPointLibrary>>& FPL)
            : ModulePass(ID), mAllocator(FPL.get_allocator()) {
            for(auto&& lib : FPL) {
                mFPTypes[lib->typeName()] = { lib->elementSize(), lib->elementAlignment() };
            }
        }
        bool runOnModule(llvm::Module& M) override {
            bool modified = false;
            // TODO:modify structures which contain PIPER_FP
            std::unordered_map<llvm::Type*, std::pair<size_t, size_t>> info(mFPTypes.get_allocator());
            for(auto structure : M.getIdentifiedStructTypes()) {
                auto name = structure->getName();
                if(!name.endswith("_PIPER_FP"))
                    continue;
                auto type = name.substr(7, name.size() - 16);
                auto key = String{ type.data(), type.size(), mAllocator };
                auto iter = mFPTypes.find(key);
                if(iter == mFPTypes.cend())
                    throw;
                auto unit = llvm::Type::getInt8Ty(M.getContext());
                auto arr = llvm::ArrayType::get(unit, iter->second.first);
                // TODO:alignment
                structure->setBody(llvm::ArrayRef<llvm::Type*>{ arr });
            }
            for(auto&& func : M.getFunctionList()) {
                for(auto&& block : func.getBasicBlockList()) {
                    for(auto&& inst : block.getInstList()) {
                        // alloca/load/store/memcpy
                        switch(inst.getOpcode()) {
                            case llvm::Instruction::Alloca: {
                                auto type = inst.getOperand(0)->getType();
                                auto iter = info.find(type);
                                if(iter != info.cend()) {
                                    llvm::dyn_cast<llvm::AllocaInst>(&inst)->setAlignment(
                                        llvm::MaybeAlign{ llvm::Align{ iter->second.second } });
                                    modified = true;
                                }
                            } break;
                            case llvm::Instruction::Load: {
                                auto type = inst.getOperand(0)->getType();
                                auto iter = info.find(type);
                                if(iter != info.cend()) {
                                    llvm::dyn_cast<llvm::LoadInst>(&inst)->setAlignment(
                                        llvm::MaybeAlign{ llvm::Align{ iter->second.second } });
                                    modified = true;
                                }
                            } break;
                            case llvm::Instruction::Store: {
                                auto type = inst.getOperand(0)->getType();
                                auto iter = info.find(type);
                                if(iter != info.cend()) {
                                    llvm::dyn_cast<llvm::StoreInst>(&inst)->setAlignment(
                                        llvm::MaybeAlign{ llvm::Align{ iter->second.second } });
                                    modified = true;
                                }
                            }
                            case llvm::Instruction::Call: {
                                auto call = llvm::dyn_cast<llvm::CallInst>(&inst);
                                if(call->getCalledFunction()->getName() == "llvm.memcpy.p0i8.p0i8.i64") {
                                    auto address = call->getArgOperand(0);
                                    auto cast = llvm::dyn_cast<llvm::BitCastInst>(address);
                                    if(cast) {
                                        auto type = cast->getType();
                                        auto iter = info.find(type);
                                        if(iter != info.cend()) {
                                            call->setArgOperand(
                                                2, llvm::ConstantInt::get(call->getParamByValType(2), iter->second.first));
                                            modified = true;
                                        }
                                    }
                                }
                            }
                            default:
                                break;
                        }
                    }
                }
            }
            return modified;
        }
    };

    class SuffixAppender final : public llvm::ModulePass {
    private:
        String mSuffix;
        static char ID;

    public:
        explicit SuffixAppender(String suffix) : ModulePass(ID), mSuffix(std::move(suffix)) {}
        bool runOnModule(llvm::Module& M) override {
            for(auto&& F : M.getFunctionList())
                F.setName(F.getName() + mSuffix.c_str());
            return true;
        }
    };

    char FPReplacer::ID;
    char SuffixAppender::ID;

    class LLVMStream final : public llvm::raw_pwrite_stream {
    private:
        Vector<std::byte>& mData;

    public:
        explicit LLVMStream(Vector<std::byte>& data) : raw_pwrite_stream(true), mData(data) {}
        void pwrite_impl(const char* ptr, size_t size, uint64_t offset) override {
            if(offset != mData.size())
                throw;
            write_impl(ptr, size);
        }
        void write_impl(const char* ptr, size_t size) override {
            auto beg = reinterpret_cast<const std::byte*>(ptr), end = beg + size;
            mData.insert(mData.cend(), beg, end);
        }
        uint64_t current_pos() const override {
            return mData.size();
        }
    };

    class LLVMIR final : public PITU, public eastl::enable_shared_from_this<LLVMIR> {
    private:
        mutable SharedPtr<llvm::LLVMContext> mContext;
        mutable std::unique_ptr<llvm::Module> mModule;
        mutable std::mutex mMutex;

    public:
        LLVMIR(PiperContext& context, SharedPtr<llvm::LLVMContext> llvmctx, std::unique_ptr<llvm::Module> module)
            : PITU(context), mContext(std::move(llvmctx)), mModule(std::move(module)) {}
        // TODO:reduce clone
        std::unique_ptr<llvm::Module> cloneModule() const {
            return llvm::CloneModule(*mModule);
        }

        Pair<Future<Vector<std::byte>>, CString>
        generateLinkable(const Span<const CString>& acceptableFormat,
                         const Vector<Future<SharedPtr<FloatingPointLibrary>>>& FPL) const override {
            auto& scheduler = context().getScheduler();
            auto complete = scheduler.spawn(
                [mod = cloneModule()](Future<Vector<SharedPtr<FloatingPointLibrary>>> FPL) {
                    auto replacer = std::make_unique<FPReplacer>(FPL.get());

                    llvm::legacy::PassManager pass;
                    pass.add(replacer.release());
                    pass.run(*mod);

                    return std::move(const_cast<std::unique_ptr<llvm::Module>&>(mod));
                },
                scheduler.wrap(FPL));
            for(auto&& format : acceptableFormat) {
                // TODO:LLVM IR Version
                if(StringView{ format } == "LLVM IR") {
                    // TODO:lazy parse linkable and directly return data
                    return makePair(context().getScheduler().spawn(
                                        [ctx = &context()](Future<std::unique_ptr<llvm::Module>> mod) {
                                            Vector<std::byte> data{ ctx->getAllocator() };
                                            LLVMStream stream(data);
                                            llvm::WriteBitcodeToFile(*mod.get(), stream);
                                            stream.flush();
                                            return std::move(data);
                                        },
                                        complete),
                                    format);
                }
                std::string error;
                auto target = llvm::TargetRegistry::lookupTarget(format, error);
                if(target) {
                    return makePair(scheduler.spawn(
                                        [target, format, ctx = &context()](Future<std::unique_ptr<llvm::Module>> mod) {
                                            auto stage = ctx->getErrorHandler().enterStageStatic("generate linkable",
                                                                                                 PIPER_SOURCE_LOCATION());
                                            // TODO:llvm::sys::getHostCPUName();llvm::sys::getHostCPUFeatures();
                                            auto cpu = "generic";
                                            auto features = "";

                                            llvm::TargetOptions opt;
                                            auto RM = llvm::Optional<llvm::Reloc::Model>();
                                            auto targetMachine = target->createTargetMachine(format, cpu, features, opt, RM);
                                            mod.get()->setDataLayout(targetMachine->createDataLayout());
                                            mod.get()->setTargetTriple(format);

                                            llvm::legacy::PassManager pass;
                                            Vector<std::byte> data{ ctx->getAllocator() };
                                            LLVMStream stream(data);
                                            if(!(targetMachine->addPassesToEmitFile(pass, stream, nullptr,
                                                                                    llvm::CodeGenFileType::CGFT_ObjectFile)))
                                                ctx->getErrorHandler().raiseException("Failed to create object emit pass",
                                                                                      PIPER_SOURCE_LOCATION());
                                            pass.run(*mod.get());
                                            stream.flush();
                                            return std::move(data);
                                        },
                                        complete),
                                    format);
                }
            }
            // TODO:llvm::TargetRegistry::printRegisteredTargetsForVersion();
            context().getErrorHandler().raiseException("No supported linkable program format", PIPER_SOURCE_LOCATION());
        }
    };
    class LLVMIRManager final : public PITUManager {
    private:
        SharedPtr<llvm::LLVMContext> mContext;

    public:
        explicit LLVMIRManager(PiperContext& context)
            : PITUManager(context), mContext(makeSharedPtr<llvm::LLVMContext>(context.getAllocator())) {}
        Future<SharedPtr<PITU>> loadPITU(const String& path) const override {
            return context().getScheduler().spawn([ctx = &context(), path, llvmctx = mContext] {
                auto stage = ctx->getErrorHandler().enterStage("load PITU " + path, PIPER_SOURCE_LOCATION());
                auto file = ctx->getFileSystem().mapFile(path, FileAccessMode::Read, FileCacheHint::Sequential);
                auto map = file->map(0, file->size());
                auto span = map->get();
                auto beg = reinterpret_cast<uint8_t*>(span.data()), end = beg + span.size();
                stage.switchToStatic("parse LLVM Bitcode", PIPER_SOURCE_LOCATION());
                auto res = llvm::parseBitcodeFile(
                    llvm::MemoryBufferRef{ toStringRef(llvm::ArrayRef<uint8_t>{ beg, end }), "bitcode data" }, *llvmctx);
                if(res)
                    return eastl::static_shared_pointer_cast<PITU>(makeSharedObject<LLVMIR>(*ctx, llvmctx, std::move(res.get())));
                auto error = toString(res.takeError());
                ctx->getErrorHandler().raiseException(StringView{ error.c_str(), error.size() }, PIPER_SOURCE_LOCATION());
            });
        }
        Future<SharedPtr<PITU>> mergePITU(const Future<Vector<SharedPtr<PITU>>>& pitus) const override {
            return context().getScheduler().spawn(
                [ctx = &context(), llvmctx = mContext](const Future<Vector<SharedPtr<PITU>>>& pitus) {
                    auto stage = ctx->getErrorHandler().enterStageStatic("link LLVM modules", PIPER_SOURCE_LOCATION());

                    // TODO:module ID param
                    auto module = std::make_unique<llvm::Module>("merged module", *llvmctx);
                    llvm::Linker linker(*module);

                    for(auto& mod : *pitus) {
                        auto ir = dynamic_cast<const LLVMIR*>(mod.get());
                        if(!ir)
                            ctx->getErrorHandler().raiseException("Unsupported PITU", PIPER_SOURCE_LOCATION());
                        linker.linkInModule(ir->cloneModule());
                    }

                    return eastl::static_shared_pointer_cast<PITU>(makeSharedObject<LLVMIR>(*ctx, llvmctx, std::move(module)));
                },
                pitus);
        }
        Future<SharedPtr<PITU>> appendSuffix(const Future<SharedPtr<PITU>>& pitu, const String& suffix) const override {
            return context().getScheduler().spawn(
                [ctx = &context(), llvmctx = mContext, suffix](const Future<SharedPtr<PITU>>& pitu) {
                    // TODO:module ID param
                    auto ir = dynamic_cast<const LLVMIR*>(pitu.get().get());
                    if(!ir)
                        ctx->getErrorHandler().raiseException("Unsupported PITU", PIPER_SOURCE_LOCATION());
                    auto mod = ir->cloneModule();
                    auto appender = std::make_unique<SuffixAppender>(suffix);

                    llvm::legacy::PassManager pass;
                    pass.add(appender.release());
                    pass.run(*mod);
                    return eastl::static_shared_pointer_cast<PITU>(makeSharedObject<LLVMIR>(*ctx, llvmctx, std::move(mod)));
                },
                pitu);
        }
    };
    class ModuleImpl final : public Module {
    public:
        explicit ModuleImpl(PiperContext& context, const char*) : Module(context) {
            if(!llvm::llvm_is_multithreaded())
                throw;
            // TODO:reduce
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmParsers();
            llvm::InitializeAllAsmPrinters();
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "LLVMIRManager") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<LLVMIRManager>(context())));
            }
            throw;
        }
        ~ModuleImpl() noexcept {
            llvm::llvm_shutdown();
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
