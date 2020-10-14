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
#include "Interface/Infrastructure/Allocator.hpp"
#include "Interface/Infrastructure/Concurrency.hpp"
#include "Interface/Infrastructure/ErrorHandler.hpp"
#include "Interface/Infrastructure/FileSystem.hpp"
#include "Interface/Infrastructure/Logger.hpp"
#include "Interface/Infrastructure/Module.hpp"
#include "Interface/Infrastructure/PhysicalQuantitySIDesc.hpp"
#include "PiperAPI.hpp"
#include "PiperContext.hpp"
#include "STL/GSL.hpp"
#include "STL/Pair.hpp"
#include "STL/SharedPtr.hpp"
#include "STL/Stack.hpp"
#include "STL/String.hpp"
#include "STL/UMap.hpp"
#include "STL/USet.hpp"
#include "STL/UniquePtr.hpp"
#include <charconv>
#include <iostream>
#include <regex>
// use builtin allocator
#include <cassert>
#include <filesystem>
#include <mutex>
#include <shared_mutex>

namespace fs = std::filesystem;

PIPER_API int EA::StdC::Vsnprintf(char* EA_RESTRICT pDestination, size_t n, const char* EA_RESTRICT pFormat, va_list arguments) {
    return vsnprintf(pDestination, n, pFormat, arguments);
}

void* allocMemory(size_t alignment, size_t size);
void freeMemory(void* ptr) noexcept;
void freeModule(void* handle);
void* loadModule(const fs::path& path);
void* getModuleSymbol(void* handle, const Piper::CString symbol);
Piper::CString getModuleExtension();
void nativeFileSystem(Piper::PiperContextOwner& context);

namespace Piper {
    StageGuard::~StageGuard() noexcept {
        mHandler.exitStage();
    }
    void StageGuard::switchTo(const String& stage, const SourceLocation& loc) {
        mHandler.exitStage();
        mHandler.enterStageImpl(stage, loc);
    }
    // TODO:overload
    void StageGuard::switchToStatic(const CString stage, const SourceLocation& loc) {
        mHandler.exitStage();
        // TODO:no allocation stroage
        mHandler.enterStageImpl(String(stage, mHandler.context().getAllocator()), loc);
    }
    // TODO:flags
    void* STLAllocator::allocate(size_t n, int flags) {
        return reinterpret_cast<void*>(mAllocator->alloc(n));
    }
    void* STLAllocator::allocate(size_t n, size_t alignment, size_t offset, int flags) {
        if(offset != 0)
            throw;
        return reinterpret_cast<void*>(mAllocator->alloc(n, alignment));
    }
    void STLAllocator::deallocate(void* p, size_t) noexcept {
        mAllocator->free(reinterpret_cast<Ptr>(p));
    }

    class DefaultAllocator final : public Allocator {
    public:
        explicit DefaultAllocator(PiperContext& view) : Allocator(view) {}
        Ptr alloc(const size_t size, const size_t align) override {
            static_assert(sizeof(Ptr) == sizeof(void*));
            auto res = reinterpret_cast<Ptr>(allocMemory(align, size));
            if(res == 0)
                throw;  // TODO:bad_alloc
            return res;
        }
        void free(const Ptr ptr) noexcept override {
            freeMemory(reinterpret_cast<void*>(ptr));
        }
    };

    class UnitManagerImpl final : public UnitManager {
    private:
        struct Hasher final {
            size_t operator()(const PhysicalQuantitySIDesc& desc) const {
                // FNV1-a
                uint64_t res = 14695981039346656037ULL;
                auto ptr = reinterpret_cast<const uint8_t*>(&desc);
                auto end = reinterpret_cast<const uint8_t*>(&desc) + sizeof(desc);
                while(ptr != end) {
                    res = (res ^ (*ptr)) * 1099511628211ULL;
                    ++ptr;
                }

                static_assert(std::is_same_v<uint64_t, size_t>);
                return res;
            }
        };
        UMap<PhysicalQuantitySIDesc, String, Hasher> mD2S;
        UMap<String, PhysicalQuantitySIDesc> mS2D;

        String serializeSI(const PhysicalQuantitySIDesc& desc) const {
            String res{ context().getAllocator() };
            auto flag = false;
#define Unit(x)                                                \
    if(desc.x) {                                               \
        if(flag)                                               \
            res += '*';                                        \
        else                                                   \
            flag = true;                                       \
        res += #x;                                             \
        if(desc.x != 1) {                                      \
            res += '^';                                        \
            res += toString(context().getAllocator(), desc.x); \
        }                                                      \
    }

            Unit(m);
            Unit(kg);
            Unit(s);
            Unit(A);
            Unit(K);
            Unit(mol);
            Unit(cd);
            Unit(rad);
            Unit(sr);

#undef Unit
            return res;
        }
        String serializeUnit(const PhysicalQuantitySIDesc& desc, const bool forceUseSIUnit) const {
            if(forceUseSIUnit)
                return serializeSI(desc);
            const auto iter = mD2S.find(desc);
            return iter == mD2S.cend() ? serializeSI(desc) : iter->second;
        }

        std::regex mRegex;

    public:
        explicit UnitManagerImpl(PiperContext& context)
            : UnitManager(context), mD2S(context.getAllocator()), mS2D(context.getAllocator()),
              mRegex("([A-Za-z]+)(\\^-?[1-9][0-9]*)?", std::regex::flag_type::ECMAScript | std::regex::flag_type::optimize) {
            mS2D.insert(makePair(String("m", context.getAllocator()), PhysicalQuantitySIDesc{ 1, 0, 0, 0, 0, 0, 0, 0, 0 }));
            mS2D.insert(makePair(String("kg", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 1, 0, 0, 0, 0, 0, 0, 0 }));
            mS2D.insert(makePair(String("s", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 1, 0, 0, 0, 0, 0, 0 }));
            mS2D.insert(makePair(String("A", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 0, 1, 0, 0, 0, 0, 0 }));
            mS2D.insert(makePair(String("K", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 0, 0, 1, 0, 0, 0, 0 }));
            mS2D.insert(makePair(String("mol", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 0, 0, 0, 1, 0, 0, 0 }));
            mS2D.insert(makePair(String("cd", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 0, 0, 0, 0, 1, 0, 0 }));
            mS2D.insert(makePair(String("rad", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 0, 0, 0, 0, 0, 1, 0 }));
            mS2D.insert(makePair(String("sr", context.getAllocator()), PhysicalQuantitySIDesc{ 0, 0, 0, 0, 0, 0, 0, 0, 1 }));
        }
        void addTranslation(const PhysicalQuantitySIDesc& desc, const StringView& name) override {
            if(mD2S.count(desc))
                throw;
            String str{ name, context().getAllocator() };
            if(mS2D.count(str) != 0)
                throw;
            mD2S.insert(makePair(desc, str));
            mS2D.insert(makePair(str, desc));
        }
        String serialize(const PhysicalQuantitySIDesc& desc, const bool forceUseSIUnit) const override {
            return serializeUnit(desc, forceUseSIUnit);
        }
        PhysicalQuantitySIDesc deserialize(const StringView& name) const override {
            PhysicalQuantitySIDesc desc{};
            Index lastPos = 0;
            for(Index i = 0; i <= name.size(); ++i) {
                if(i != name.size() && name[i] != '*')
                    continue;
                std::cmatch matches;
                bool res = std::regex_match(name.cbegin() + lastPos, name.cbegin() + i, matches, mRegex);
                if(!res)
                    throw;
                if(matches.size() < 2 || matches.size() > 3)
                    throw;
                const auto iter = mS2D.find(String(matches[1].first, matches[1].second, context().getAllocator()));
                if(iter == mS2D.cend())
                    throw;
                int32_t base = 1;
                if(matches.size() == 3 && matches[2].matched) {
                    auto res = std::from_chars(matches[2].first + 1, matches[2].second, base);
                    Ensures(res.ptr == matches[2].second);
                }
#define Unit(x) desc.x += iter->second.x * base;

                Unit(m);
                Unit(kg);
                Unit(s);
                Unit(A);
                Unit(K);
                Unit(mol);
                Unit(cd);
                Unit(rad);
                Unit(sr);

#undef Unit
                lastPos = i + 1;
            }
            return desc;
        }
    };

    class DLLHandle final {
    private:
        struct HandleCloser {
            void operator()(void* handle) const {
                freeModule(handle);
            }
        };
        UniquePtr<void, HandleCloser> mHandle;

    public:
        explicit DLLHandle(void* handle) : mHandle(handle) {}
        void* getFunctionAddress(const CString symbol) const {
            return getModuleSymbol(mHandle.get(), symbol);
        }
    };

    class ModuleLoaderImpl final : public ModuleLoader {
    private:
        UMap<String, SharedPtr<Module>> mModules;
        UMap<String, Pair<SharedPtr<Config>, String>> mModuleDesc;
        Stack<DLLHandle> mHandles;
        // USet<String> mClasses; TODO:Cache
        std::shared_mutex mMutex;

    public:
        explicit ModuleLoaderImpl(PiperContext& context)
            : ModuleLoader(context), mModules(context.getAllocator()), mHandles(STLAllocator{ context.getAllocator() }),
              mModuleDesc(context.getAllocator()) {}
        Future<void> loadModule(const SharedPtr<Config>& moduleDesc, const String& descPath) override {
            auto stage = context().getErrorHandler().enterStageStatic("parse package description", PIPER_SOURCE_LOCATION());
            auto&& info = moduleDesc->viewAsObject();
            auto iter = info.find(String("Name", context().getAllocator()));
            if(iter == info.cend())
                throw;
            auto name = iter->second->get<String>();

            {
                std::shared_lock guard{ mMutex };
                if(mModules.count(name))
                    return context().getScheduler().ready();
            }
            // TODO:double checked locking

            iter = info.find(String("Path", context().getAllocator()));
            if(iter == info.cend())
                throw;
            auto base = descPath + "/";
            auto path = iter->second->get<String>();

            iter = info.find("Dependencies");
            Vector<String> deps(context().getAllocator());
            if(iter != info.cend()) {
                for(auto&& dep : iter->second->viewAsArray())
                    deps.push_back(dep->get<String>());
            }
            // TODO:filesystem

            stage.switchTo("spawn load " + name, PIPER_SOURCE_LOCATION());

            return context().getScheduler().spawn([this, base, name, path, deps] {
                // TODO:checksum:blake2sp
                // TODO:module dependences/moduleDesc provider
                for(auto&& dep : deps) {
                    auto stage =
                        context().getErrorHandler().enterStage("load third-party dependence " + dep, PIPER_SOURCE_LOCATION());
                    auto handle = reinterpret_cast<void*>(::loadModule(fs::u8path((base + dep + getModuleExtension()).c_str())));
                    {
                        std::unique_lock<std::shared_mutex> guard{ mMutex };
                        mHandles.push(DLLHandle{ handle });
                    }
                }
                auto stage = context().getErrorHandler().enterStage("load module " + path, PIPER_SOURCE_LOCATION());
                auto handle = reinterpret_cast<void*>(::loadModule(fs::u8path((base + path + getModuleExtension()).c_str())));
                DLLHandle lib{ handle };
                stage.switchToStatic("check module protocol", PIPER_SOURCE_LOCATION());
                static const StringView coreProtocol = PIPER_ABI "@" PIPER_STL "@" PIPER_INTERFACE;
                using ProtocolFunc = const char* (*)();
                auto protocol = reinterpret_cast<ProtocolFunc>(lib.getFunctionAddress("piperGetProtocol"));
                if(StringView{ protocol() } != coreProtocol)
                    throw;
                stage.switchToStatic("init module", PIPER_SOURCE_LOCATION());
                using InitFunc = Module* (*)(PiperContext & context, Allocator & allocator);
                auto init = reinterpret_cast<InitFunc>(lib.getFunctionAddress("piperInitModule"));
                auto& allocator = context().getAllocator();
                auto mod = SharedPtr<Module>{ init(context(), allocator), DefaultDeleter<Module>{ allocator },
                                              STLAllocator{ allocator } };
                if(!mod)
                    throw;
                {
                    std::unique_lock<std::shared_mutex> guard{ mMutex };
                    mModules.insert(makePair(name, mod));
                    mHandles.push(std::move(lib));
                }
            });
        };
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            // TODO:asynchronous module loading
            module.wait();

            auto stage = context().getErrorHandler().enterStage("new instance of " + String(classID, context().getAllocator()),
                                                                PIPER_SOURCE_LOCATION());
            const auto pos = classID.find_last_of('.');
            if(pos == String::npos)
                throw;

            std::shared_lock<std::shared_mutex> guard{ mMutex };
            const auto iter = mModules.find(String(classID.substr(0, pos), context().getAllocator()));
            if(iter == mModules.cend())
                throw;
            return iter->second->newInstance(classID.substr(pos + 1), config, module);
        }
        Future<void> loadModule(const String& moduleID) override {
            if(mModules.count(moduleID))
                return context().getScheduler().ready();
            // TODO:lock module desc
            auto iter = mModuleDesc.find(moduleID);
            if(iter == mModuleDesc.cend())
                throw;
            return loadModule(iter->second.first, iter->second.second);
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config) override {
            auto stage = context().getErrorHandler().enterStage("new instance of " + String(classID, context().getAllocator()),
                                                                PIPER_SOURCE_LOCATION());
            const auto pos = classID.find_last_of('.');
            if(pos == String::npos)
                throw;

            auto module = loadModule(String{ classID.substr(0, pos), context().getAllocator() });
            // TODO:reduce classID split time
            return newInstance(classID, config, module);
        }
        void addModuleDescription(const SharedPtr<Config>& moduleDesc, const String& descPath) override {
            auto&& obj = moduleDesc->viewAsObject();
            auto iter = obj.find(String{ "Name", context().getAllocator() });
            if(iter == obj.cend())
                throw;
            mModuleDesc.insert(makePair(iter->second->get<String>(), makePair(moduleDesc, descPath)));
        }

        ~ModuleLoaderImpl() {
            // TODO:fix destroy order in context
            // auto stage = context().getErrorHandler().enterStage("destroy modules", PIPER_SOURCE_LOCATION());
            // TODO:destroy order
            mModuleDesc.clear();
            mModules.clear();
            while(!mHandles.empty())
                mHandles.pop();
        }
    };

    class LoggerImpl final : public Logger {
    private:
        CString level2Str(const LogLevel level) const noexcept {
            switch(level) {
                case LogLevel::Info:
                    return "INFO";
                case LogLevel::Warning:
                    return "WARNING";
                case LogLevel::Error:
                    return "ERROR";
                case LogLevel::Fatal:
                    return "FATAL";
                case LogLevel::Debug:
                    return "DEBUG";
                default:
                    return "UNKNOWN";
            }
        }
        std::mutex mMutex;

    public:
        PIPER_INTERFACE_CONSTRUCT(LoggerImpl, Logger)
        bool allow(const LogLevel level) const noexcept override {
            return true;
        }
        void record(const LogLevel level, const StringView& message, const SourceLocation& sourceLocation) noexcept override {
            std::lock_guard guard(mMutex);
            std::cerr << "[" << level2Str(level) << "]" << std::string_view(message.data(), message.size()) << std::endl;
            std::cerr << "<<<< " << sourceLocation.func << "[" << sourceLocation.file << ":" << sourceLocation.line << "]"
                      << std::endl;
        }
        void flush() noexcept override {
            std::cerr.flush();
        }
    };

    class FutureStorage final : public FutureImpl {
    private:
        void* mPtr;

        void* alloc(PiperContext& context, const size_t size) {
            if(size)
                return reinterpret_cast<void*>(context.getAllocator().alloc(size));
            return nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size) : FutureImpl(context), mPtr(alloc(context, size)) {}
        bool ready() const noexcept override {
            return true;
        }
        bool fastReady() const noexcept override {
            return true;
        }
        const void* storage() const override {
            return mPtr;
        }
        void wait() const override {}
    };

    class SchedulerImpl final : public Scheduler {
    public:
        PIPER_INTERFACE_CONSTRUCT(SchedulerImpl, Scheduler)

        void spawnImpl(Optional<Closure<>> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                       const SharedPtr<FutureImpl>& res) override {
            for(auto&& dep : dependencies)
                if(dep)
                    dep->wait();
            if(func.has_value())
                func.value()();
        }
        void parallelForImpl(uint32_t n, Closure<uint32_t> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                             const SharedPtr<FutureImpl>& res) override {
            for(auto&& dep : dependencies)
                if(dep)
                    dep->wait();
            for(uint32_t i = 0; i < n; ++i)
                func(i);
        }
        SharedPtr<FutureImpl> newFutureImpl(const size_t size, const bool) override {
            return makeSharedObject<FutureStorage>(context(), size);
        }
    };

    class ErrorHandlerImpl final : public ErrorHandler {
    private:
        UMap<std::thread::id, Stack<Pair<String, SourceLocation>>, std::hash<std::thread::id>> mStages;
        std::shared_mutex mMutex;
        void beforeAbort() {}
        auto& locate() {
            auto id = std::this_thread::get_id();
            {
                std::shared_lock<std::shared_mutex> guard{ mMutex };
                auto iter = mStages.find(id);
                if(iter != mStages.cend())
                    return iter->second;
            }
            std::unique_lock<std::shared_mutex> guard{ mMutex };
            auto iter = mStages.find(id);
            if(iter != mStages.cend())
                return iter->second;
            return mStages
                .insert(makePair(
                    id,
                    decltype(mStages)::mapped_type{ decltype(mStages)::mapped_type::container_type{ context().getAllocator() } }))
                .first->second;
        }

    public:
        explicit ErrorHandlerImpl(PiperContext& context) : ErrorHandler(context), mStages(context.getAllocator()) {}

        // for runtime error
        void raiseException(const StringView& message, const SourceLocation& loc) override {
            context().getLogger().record(LogLevel::Fatal, message, loc);
            context().getLogger().flush();
            beforeAbort();
            std::abort();
        }

        bool allowAssert(const CheckLevel level) override {
            notImplemented(PIPER_SOURCE_LOCATION());
        }
        void assertFailed(const CheckLevel level, const CString expression, const SourceLocation& loc) override {
            notImplemented(PIPER_SOURCE_LOCATION());
        }
        void processSignal(int signal) override {
            notImplemented(PIPER_SOURCE_LOCATION());
        }
        [[noreturn]] void notImplemented(const SourceLocation& loc) override {
            throw;
        }

        void enterStageImpl(const String& stage, const SourceLocation& loc) override {
            locate().push(makePair(stage, loc));
            auto&& logger = context().getLogger();
            if(logger.allow(LogLevel::Debug))
                logger.record(LogLevel::Debug,
                              "(Stage Layer " + toString(context().getAllocator(), locate().size()) + ") " + stage, loc);
        }
        void exitStage() noexcept override {
            locate().pop();
        }
    };

    class PiperContextImpl final : public PiperContextOwner {
    private:
        DefaultAllocator mDefaultAllocator;
        Allocator* mAllocator;
        SharedPtr<Logger> mLogger;
        ModuleLoaderImpl mModuleLoader;
        SharedPtr<Scheduler> mScheduler;
        SharedPtr<FileSystem> mFileSystem;
        SharedPtr<Allocator> mUserAllocator;
        UnitManagerImpl mUnitManager;
        ErrorHandlerImpl mErrorHandler;

        Stack<SharedPtr<Object>> mLifeTimeRecorder;

    public:
        PiperContextImpl();

        Logger& getLogger() noexcept override {
            return *mLogger;
        }
        ModuleLoader& getModuleLoader() noexcept override {
            return mModuleLoader;
        }
        Scheduler& getScheduler() noexcept override {
            return *mScheduler;
        }
        FileSystem& getFileSystem() noexcept override {
            return *mFileSystem;
        }
        Allocator& getAllocator() noexcept override {
            return *mAllocator;
        }
        UnitManager& getUnitManager() noexcept override {
            return mUnitManager;
        }
        ErrorHandler& getErrorHandler() noexcept override {
            return mErrorHandler;
        }

        void setLogger(const SharedPtr<Logger>& logger) noexcept override {
            mLifeTimeRecorder.push(logger);
            mLogger = logger;
        }
        void setScheduler(const SharedPtr<Scheduler>& scheduler) noexcept override {
            mLifeTimeRecorder.push(scheduler);
            mScheduler = scheduler;
        }
        void setFileSystem(const SharedPtr<FileSystem>& filesystem) noexcept override {
            mLifeTimeRecorder.push(filesystem);
            mFileSystem = filesystem;
        }
        void setAllocator(const SharedPtr<Allocator>& allocator) noexcept override {
            mLifeTimeRecorder.push(allocator);
            mUserAllocator = allocator;
            mAllocator = mUserAllocator.get();
        }
        ~PiperContextImpl() {
            auto stage = mErrorHandler.enterStageStatic("destroy Piper context", PIPER_SOURCE_LOCATION());

            mFileSystem.reset();
            mScheduler.reset();
            // BUG:some contaniers of ErrorHandler and ModuleLoader are using user allocator.
            // mUserAllocator.reset();
            mLogger->flush();
            mLogger.reset();

            while(!mLifeTimeRecorder.empty())
                mLifeTimeRecorder.pop();
        }
    };

    PiperContextImpl::PiperContextImpl()
        : mDefaultAllocator(*this), mAllocator(&mDefaultAllocator), mModuleLoader(*this), mUnitManager(*this),
          mErrorHandler(*this), mLifeTimeRecorder(STLAllocator{ getAllocator() }) {
        setLogger(makeSharedObject<LoggerImpl>(*this));
        auto stage = mErrorHandler.enterStageStatic("create Piper context", PIPER_SOURCE_LOCATION());
        setScheduler(makeSharedObject<SchedulerImpl>(*this));
        nativeFileSystem(*this);
    }
}  // namespace Piper

PIPER_API Piper::PiperContextOwner* piperCreateContext() {
    return new Piper::PiperContextImpl();
}
PIPER_API void piperDestroyContext(Piper::PiperContextOwner* context) {
    delete context;
}
