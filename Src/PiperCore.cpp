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
#include "Interface/Infrastructure/Program.hpp"
#include "Interface/Infrastructure/ResourceUtil.hpp"
#include "PiperAPI.hpp"
#include "PiperContext.hpp"
#include "STL/GSL.hpp"
#include "STL/Pair.hpp"
#include "STL/SharedPtr.hpp"
#include "STL/String.hpp"
#include "STL/UMap.hpp"
#include "STL/USet.hpp"
#include "STL/UniquePtr.hpp"
// use builtin allocator
#include <cassert>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <regex>
#include <shared_mutex>

namespace fs = std::filesystem;

// ReSharper disable once CppInconsistentNaming
PIPER_API int EA::StdC::Vsnprintf(char* EA_RESTRICT pDestination, size_t n, const char* EA_RESTRICT pFormat, va_list arguments) {
    return vsnprintf(pDestination, n, pFormat, arguments);
}

void* allocMemory(size_t alignment, size_t size);
void freeMemory(void* ptr) noexcept;
void freeModule(Piper::PiperContext& context, void* handle);
void* loadModule(Piper::PiperContext& context, const fs::path& path);
void* getModuleSymbol(Piper::PiperContext& context, void* handle, const Piper::CString symbol);
Piper::CString getModuleExtension();
void nativeFileSystem(Piper::PiperContextOwner& context);

namespace Piper {
    ResourceHolder::ResourceHolder(PiperContext& context) : Object(context), mPool(context.getAllocator()) {}

    void ResourceHolder::retain(SharedPtr<Object> ref) {
        mPool.push_back(std::move(ref));
    }

    SharedPtr<Object> ResourceCacheManager::lookupImpl(const ResourceID id) const {
        const auto iter = mCache.find(id);
        // if(iter != mCache.cend() && !iter->second.expired())
        //    return iter->second.lock();
        if(iter != mCache.cend())
            return iter->second;
        return nullptr;
    }
    void ResourceCacheManager::reserve(const ResourceID id, const SharedPtr<Object>& cache) {
        mCache.insert_or_assign(id, SharedPtr<Object>{ cache });
    }
    ResourceCacheManager::ResourceCacheManager(PiperContext& context) : Object(context), mCache(context.getAllocator()) {}

    MemoryArena::MemoryArena(Allocator& allocator, const size_t blockSize)
        : mAllocator(allocator), mBlocks(allocator), mCurrent(0), mCurEnd(0), mBlockSize(blockSize) {}
    Ptr MemoryArena::allocRaw(const size_t size, const size_t align) {
        if(size >= mBlockSize) {
            const auto ptr = mAllocator.alloc(size, align);
            mBlocks.push_back(ptr);
            return ptr;
        }
        const auto rem = mCurrent % align;
        const auto begin = (rem == 0 ? mCurrent : mCurrent + align - rem);
        if(begin + size > mCurEnd) {
            mCurrent = mAllocator.alloc(mBlockSize, align);
            mCurEnd = mCurrent + mBlockSize;
            mBlocks.push_back(mCurrent);
            auto ptr = mCurrent;
            mCurrent += size;
            return ptr;
        }
        mCurrent = begin + size;
        return begin;
    }
    MemoryArena::~MemoryArena() {
        for(auto ptr : mBlocks)
            mAllocator.free(ptr);
    }

    StageGuard::~StageGuard() noexcept {
        mHandler.exitStage();
    }
    // ReSharper disable once CppMemberFunctionMayBeConst
    void StageGuard::next(Variant<CString, String> stage, const SourceLocation& loc) {
        mHandler.exitStage();
        mHandler.enterStageImpl(std::move(stage), loc);
    }
    // TODO:flags
    // ReSharper disable once CppMemberFunctionMayBeConst
    [[nodiscard]] void* STLAllocator::allocate(const size_t n, int flags) {
        return reinterpret_cast<void*>(mAllocator->alloc(n));
    }
    // ReSharper disable once CppMemberFunctionMayBeConst
    [[nodiscard]] void* STLAllocator::allocate(const size_t n, const size_t alignment, const size_t offset, int flags) {
        if(offset != 0)
            mAllocator->context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return reinterpret_cast<void*>(mAllocator->alloc(n, alignment));
    }
    // ReSharper disable once CppMemberFunctionMayBeConst
    [[nodiscard]] void STLAllocator::deallocate(void* p, size_t) noexcept {
        mAllocator->free(reinterpret_cast<Ptr>(p));
    }

    class DefaultAllocator final : public Allocator {
    public:
        explicit DefaultAllocator(PiperContext& view) : Allocator(view) {}
        Ptr alloc(const size_t size, const size_t align) override {
            static_assert(sizeof(Ptr) == sizeof(void*));
            const auto res = reinterpret_cast<Ptr>(allocMemory(align, size));
            if(res == 0)
                context().getErrorHandler().raiseException("Bad alloc", PIPER_SOURCE_LOCATION());
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
                auto res = 14695981039346656037ULL;
                const auto* ptr = reinterpret_cast<const uint8_t*>(&desc);
                const auto* end = reinterpret_cast<const uint8_t*>(&desc) + sizeof(desc);
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
#define PIPER_REGISTER_UNIT(x)                                 \
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

            PIPER_REGISTER_UNIT(m);
            PIPER_REGISTER_UNIT(kg);
            PIPER_REGISTER_UNIT(s);
            PIPER_REGISTER_UNIT(A);
            PIPER_REGISTER_UNIT(K);
            PIPER_REGISTER_UNIT(mol);
            PIPER_REGISTER_UNIT(cd);
            PIPER_REGISTER_UNIT(rad);
            PIPER_REGISTER_UNIT(sr);

#undef PIPER_REGISTER_UNIT
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
                return;
            String str{ name, context().getAllocator() };
            if(mS2D.count(str) != 0)
                context().getErrorHandler().raiseException("Multiple definition of units.", PIPER_SOURCE_LOCATION());
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
                const auto res = std::regex_match(name.cbegin() + lastPos, name.cbegin() + i, matches, mRegex);
                if(!res || matches.size() != 3)
                    context().getErrorHandler().raiseException("Invalid unit.", PIPER_SOURCE_LOCATION());
                String unit{ matches[1].first, matches[1].second, context().getAllocator() };
                const auto iter = mS2D.find(unit);
                if(iter == mS2D.cend())
                    context().getErrorHandler().raiseException("Undefined unit \"" + unit + "\".", PIPER_SOURCE_LOCATION());
                auto base = 1;
                if(matches.size() == 3 && matches[2].matched) {
                    auto val = std::from_chars(matches[2].first + 1, matches[2].second, base);
                    Ensures(val.ptr == matches[2].second);
                }
#define PIPER_REGISTER_UNIT(x) desc.x += (iter->second.x) * base

                PIPER_REGISTER_UNIT(m);
                PIPER_REGISTER_UNIT(kg);
                PIPER_REGISTER_UNIT(s);
                PIPER_REGISTER_UNIT(A);
                PIPER_REGISTER_UNIT(K);
                PIPER_REGISTER_UNIT(mol);
                PIPER_REGISTER_UNIT(cd);
                PIPER_REGISTER_UNIT(rad);
                PIPER_REGISTER_UNIT(sr);

#undef PIPER_REGISTER_UNIT
                lastPos = i + 1;
            }
            return desc;
        }
    };

    class DLLHandle final : public Object {
    private:
        struct HandleCloser {
            PiperContext& context;
            void operator()(void* handle) const {
                freeModule(context, handle);
            }
        };
        UniquePtr<void, HandleCloser> mHandle;

    public:
        explicit DLLHandle(PiperContext& context, void* handle) : Object(context), mHandle(handle, HandleCloser{ context }) {}
        void* getFunctionAddress(const CString symbol) const {
            return getModuleSymbol(mHandle.get_deleter().context, mHandle.get(), symbol);
        }
    };

    class PiperContextImpl;

    class ModuleLoaderImpl final : public ModuleLoader {
    private:
        UMap<String, SharedPtr<Module>> mModules;
        UMap<String, Pair<SharedPtr<Config>, String>> mModuleDesc;
        DynamicArray<SharedPtr<DLLHandle>> mHandles;
        // USet<String> mClasses; TODO:Cache
        std::recursive_mutex mMutex;  // TODO:recursive+shared?
        PiperContextImpl& mContext;

    public:
        explicit ModuleLoaderImpl(PiperContextImpl& context);
        Future<void> loadModule(const SharedPtr<Config>& moduleDesc, const String& descPath) override;
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            // TODO:asynchronous module loading
            module.wait();

            auto stage = context().getErrorHandler().enterStage("new instance of " + String(classID, context().getAllocator()),
                                                                PIPER_SOURCE_LOCATION());
            const auto pos = classID.find_last_of('.');
            if(pos == String::npos)
                context().getErrorHandler().raiseException("Invalid classID.", PIPER_SOURCE_LOCATION());

            std::unique_lock<std::recursive_mutex> guard{ mMutex };
            const String name{ classID.substr(0, pos), context().getAllocator() };
            const auto iter = mModules.find(name);
            if(iter == mModules.cend())
                context().getErrorHandler().raiseException("Module \"" + name + "\" has not been loaded.",
                                                           PIPER_SOURCE_LOCATION());
            return iter->second->newInstance(classID.substr(pos + 1), config, module);
        }
        Future<void> loadModule(const String& moduleID) override {
            if(mModules.count(moduleID))
                return context().getScheduler().ready();
            std::lock_guard<std::recursive_mutex> guard{ mMutex };
            const auto iter = mModuleDesc.find(moduleID);
            if(iter == mModuleDesc.cend())
                context().getErrorHandler().raiseException("Undefined module \"" + moduleID + "\".", PIPER_SOURCE_LOCATION());
            return loadModule(iter->second.first, iter->second.second);
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config) override {
            auto stage = context().getErrorHandler().enterStage("new instance of " + String(classID, context().getAllocator()),
                                                                PIPER_SOURCE_LOCATION());
            const auto pos = classID.find_last_of('.');
            if(pos == String::npos)
                context().getErrorHandler().raiseException(
                    "Invalid classID \"" + String{ classID, context().getAllocator() } + "\".", PIPER_SOURCE_LOCATION());

            const auto module = loadModule(String{ classID.substr(0, pos), context().getAllocator() });
            // TODO:reduce classID split time
            return newInstance(classID, config, module);
        }
        void addModuleDescription(const SharedPtr<Config>& moduleDesc, const String& descPath) override {
            std::lock_guard<std::recursive_mutex> guard{ mMutex };
            auto&& obj = moduleDesc->viewAsObject();
            const auto iter = obj.find(String{ "Name", context().getAllocator() });
            if(iter == obj.cend())
                context().getErrorHandler().raiseException("Invalid module description.", PIPER_SOURCE_LOCATION());
            mModuleDesc.insert(makePair(iter->second->get<String>(), makePair(moduleDesc, descPath)));
        }
    };

    class LoggerImpl final : public Logger {
    private:
        static CString level2Str(const LogLevel level) noexcept {
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
            }
            return "UNKNOWN";
        }

        std::mutex mMutex;

    public:
        PIPER_INTERFACE_CONSTRUCT(LoggerImpl, Logger)
        bool allow(const LogLevel level) const noexcept override {
            return true;
        }
        void record(const LogLevel level, const StringView& message, const SourceLocation& sourceLocation) noexcept override {
            std::lock_guard<std::mutex> guard(mMutex);
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

        [[nodiscard]] static void* alloc(PiperContext& context, const size_t size) {
            if(size)
                return reinterpret_cast<void*>(context.getAllocator().alloc(size));
            return nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size) : FutureImpl(context), mPtr(alloc(context, size)) {}
        [[nodiscard]] bool ready() const noexcept override {
            return true;
        }
        [[nodiscard]] bool fastReady() const noexcept override {
            return true;
        }
        [[nodiscard]] const void* storage() const override {
            return mPtr;
        }
        void wait() const override {}
    };

    class SchedulerImpl final : public Scheduler {
    public:
        PIPER_INTERFACE_CONSTRUCT(SchedulerImpl, Scheduler)

        void spawnImpl(Optional<Closure<>> func, const Span<SharedPtr<FutureImpl>>& dependencies,
                       const SharedPtr<FutureImpl>& res) override {
            for(auto&& dep : dependencies)
                if(dep)
                    dep->wait();
            if(func.has_value())
                func.value()();
        }
        void parallelForImpl(const uint32_t n, Closure<uint32_t> func, const Span<SharedPtr<FutureImpl>>& dependencies,
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
        UMap<std::thread::id, DynamicArray<Pair<Variant<CString, String>, SourceLocation>>, std::hash<std::thread::id>> mStages;
        std::shared_mutex mMutex;
        void beforeAbort() {
            notImplemented(PIPER_SOURCE_LOCATION());
        }
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
            using ValueType = decltype(mStages)::mapped_type;
            return mStages.insert(makePair(id, ValueType{ context().getAllocator() })).first->second;
        }

    public:
        explicit ErrorHandlerImpl(PiperContext& context) : ErrorHandler(context), mStages(context.getAllocator()) {}
        void setupGlobalErrorHandler() override {
            notImplemented(PIPER_SOURCE_LOCATION());
        }
        void setupThreadErrorHandler() override {
            notImplemented(PIPER_SOURCE_LOCATION());
        }

        void raiseException(const StringView& message, const SourceLocation& loc) override {
            context().getLogger().record(LogLevel::Fatal, message, loc);
            context().getLogger().flush();
            beforeAbort();
            std::abort();
        }
        void unresolvedClassID(const StringView& classID, const SourceLocation& loc) override {
            raiseException("Unresolved classID " + String{ classID, context().getAllocator() }, loc);
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
            std::abort();
        }

        [[nodiscard]] void enterStageImpl(Variant<CString, String> stage, const SourceLocation& loc) override {
            auto&& logger = context().getLogger();
            if(logger.allow(LogLevel::Debug)) {
                auto msg = "(Stage Layer " + toString(context().getAllocator(), locate().size()) + ") ";
                if(stage.index())
                    msg += eastl::get<String>(stage);
                else
                    msg += eastl::get<CString>(stage);
                logger.record(LogLevel::Debug, msg, loc);
            }
            locate().emplace_back(std::move(stage), loc);
        }
        void exitStage() noexcept override {
            locate().pop_back();
        }
    };

    class DummyPITUManager final : public PITUManager {
    public:
        PIPER_INTERFACE_CONSTRUCT(DummyPITUManager, PITUManager)
        Future<SharedPtr<PITU>> loadPITU(const String& path) const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return Future<SharedPtr<PITU>>{ nullptr };  // make compiler happy
        }
        Future<SharedPtr<PITU>> mergePITU(const Future<DynamicArray<SharedPtr<PITU>>>& pitus) const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return Future<SharedPtr<PITU>>{ nullptr };  // make compiler happy
        }
    };

    class PiperContextImpl final : public PiperContextOwner {
    private:
        DefaultAllocator mDefaultAllocator;
        Allocator* mAllocator;
        SharedPtr<Logger> mLogger;
        UniquePtr<ModuleLoaderImpl> mModuleLoader;
        SharedPtr<Scheduler> mScheduler;
        SharedPtr<FileSystem> mFileSystem;
        SharedPtr<Allocator> mUserAllocator;
        SharedPtr<PITUManager> mPITUManager;
        UniquePtr<UnitManagerImpl> mUnitManager;
        UniquePtr<ErrorHandlerImpl> mErrorHandler;
        DynamicArray<Scheduler*> mNotifyScheduler;
        std::mutex mMutex;

        DynamicArray<SharedPtr<Object>> mLifeTimeRecorder;

    public:
        PiperContextImpl();

        Logger& getLogger() noexcept override {
            return *mLogger;
        }
        ModuleLoader& getModuleLoader() noexcept override {
            return *mModuleLoader;
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
            return *mUnitManager;
        }
        ErrorHandler& getErrorHandler() noexcept override {
            return *mErrorHandler;
        }
        PITUManager& getPITUManager() noexcept override {
            return *mPITUManager;
        }

        void setLogger(SharedPtr<Logger> logger) noexcept override {
            std::lock_guard<std::mutex> guard{ mMutex };
            mLifeTimeRecorder.emplace_back(logger);
            mLogger = std::move(logger);
        }
        void setScheduler(SharedPtr<Scheduler> scheduler) noexcept override {
            std::lock_guard<std::mutex> guard{ mMutex };
            mLifeTimeRecorder.emplace_back(scheduler);
            if(scheduler->supportNotify())
                mNotifyScheduler.push_back(scheduler.get());
            mScheduler = std::move(scheduler);
        }
        void setFileSystem(SharedPtr<FileSystem> filesystem) noexcept override {
            std::lock_guard<std::mutex> guard{ mMutex };
            mLifeTimeRecorder.emplace_back(filesystem);
            mFileSystem = std::move(filesystem);
        }
        void setAllocator(SharedPtr<Allocator> allocator) noexcept override {
            std::lock_guard<std::mutex> guard{ mMutex };
            mLifeTimeRecorder.emplace_back(allocator);
            mUserAllocator = std::move(allocator);
            mAllocator = mUserAllocator.get();
        }
        void setPITUManager(SharedPtr<PITUManager> manager) noexcept override {
            std::lock_guard<std::mutex> guard{ mMutex };
            mLifeTimeRecorder.emplace_back(manager);
            mPITUManager = std::move(manager);
        }

        void notify(FutureImpl* event) override {
            for(auto&& scheduler : mNotifyScheduler)
                scheduler->notify(event);
        }

        void registerDelegatedObject(SharedPtr<Object> object) {
            std::lock_guard<std::mutex> guard{ mMutex };
            mLifeTimeRecorder.emplace_back(std::move(object));
        }

        ~PiperContextImpl() noexcept override {
            if(mLogger->allow(LogLevel::Info))
                mLogger->record(LogLevel::Info, "destroy Piper context", PIPER_SOURCE_LOCATION());
            mLogger->flush();

            mLogger.reset();
            mFileSystem.reset();
            mScheduler.reset();
            mUnitManager.reset();
            mUserAllocator.reset();
            mErrorHandler.reset();
            mPITUManager.reset();
            mModuleLoader.reset();

            while(!mLifeTimeRecorder.empty())
                mLifeTimeRecorder.pop_back();
        }
    };

    PiperContextImpl::PiperContextImpl()
        : mDefaultAllocator(*this), mAllocator(&mDefaultAllocator),
          mModuleLoader(makeUniquePtr<ModuleLoaderImpl>(mDefaultAllocator, *this)),
          mUnitManager(makeUniquePtr<UnitManagerImpl>(mDefaultAllocator, *this)),
          mErrorHandler(makeUniquePtr<ErrorHandlerImpl>(mDefaultAllocator, *this)), mNotifyScheduler(getAllocator()),
          mLifeTimeRecorder(STLAllocator{ getAllocator() }) {
        setLogger(makeSharedObject<LoggerImpl>(*this));
        auto stage = mErrorHandler->enterStage("create Piper context", PIPER_SOURCE_LOCATION());
        setScheduler(makeSharedObject<SchedulerImpl>(*this));
        nativeFileSystem(*this);
        mPITUManager = makeSharedObject<DummyPITUManager>(*this);
    }

    ModuleLoaderImpl::ModuleLoaderImpl(PiperContextImpl& context)
        : ModuleLoader(context), mModules(context.getAllocator()), mModuleDesc(context.getAllocator()),
          mHandles(STLAllocator{ context.getAllocator() }), mContext(context) {}

    Future<void> ModuleLoaderImpl::loadModule(const SharedPtr<Config>& moduleDesc, const String& descPath) {
        auto stage = context().getErrorHandler().enterStage("parse package description", PIPER_SOURCE_LOCATION());
        auto&& info = moduleDesc->viewAsObject();
        auto iter = info.find(String("Name", context().getAllocator()));
        if(iter == info.cend())
            context().getErrorHandler().raiseException("Module's name is needed.", PIPER_SOURCE_LOCATION());
        auto name = iter->second->get<String>();

        {
            std::unique_lock<std::recursive_mutex> guard{ mMutex };
            if(mModules.count(name))
                return context().getScheduler().ready();
        }
        // TODO:double checked locking

        iter = info.find(String("Path", context().getAllocator()));
        if(iter == info.cend())
            context().getErrorHandler().raiseException("Module's path is needed.", PIPER_SOURCE_LOCATION());
        const auto path = descPath + "/" + iter->second->get<String>();

        iter = info.find("Dependencies");
        DynamicArray<String> deps(context().getAllocator());
        if(iter != info.cend()) {
            for(auto&& dep : iter->second->viewAsArray())
                deps.push_back(dep->get<String>());
        }
        // TODO:filesystem

        stage.next("spawn load " + name, PIPER_SOURCE_LOCATION());

        // TODO:concurrency with mutex
        // return context().getScheduler().spawn([this, name, path, deps, descPath] {
        // TODO:checksum:blake2sp
        // TODO:module dependencies
        for(auto&& dep : deps) {
            auto stage2 = context().getErrorHandler().enterStage("load third-party dependence " + dep, PIPER_SOURCE_LOCATION());
            auto* handle =
                static_cast<void*>(::loadModule(context(), fs::u8path((descPath + "/" + dep + getModuleExtension()).c_str())));
            {
                std::unique_lock<std::recursive_mutex> guard{ mMutex };
                auto managedHandle = makeSharedObject<DLLHandle>(context(), handle);
                mHandles.emplace_back(managedHandle);
                mContext.registerDelegatedObject(std::move(managedHandle));
            }
        }
        {
            auto stage3 = context().getErrorHandler().enterStage("load module " + path, PIPER_SOURCE_LOCATION());
            const auto moduleFullPath = fs::u8path((path + getModuleExtension()).c_str());
            auto* handle = static_cast<void*>(::loadModule(context(), moduleFullPath));
            auto lib = makeSharedObject<DLLHandle>(context(), handle);
            stage.next("check module protocol", PIPER_SOURCE_LOCATION());
            static const StringView coreProtocol = PIPER_ABI "@" PIPER_STL "@" PIPER_INTERFACE;
            using ProtocolFunc = CString (*)();
            const auto protocol = static_cast<ProtocolFunc>(lib->getFunctionAddress("piperGetProtocol"));
            if(StringView{ protocol() } != coreProtocol)
                context().getErrorHandler().raiseException("Module protocols mismatched.", PIPER_SOURCE_LOCATION());
            stage.next("init module", PIPER_SOURCE_LOCATION());
            using InitFunc = Module* (*)(PiperContext & context, Allocator & allocator, CString);
            const auto init = static_cast<InitFunc>(lib->getFunctionAddress("piperInitModule"));
            auto& allocator = context().getAllocator();
            auto mod =
                SharedPtr<Module>{ init(context(), allocator, fs::absolute(moduleFullPath).parent_path().u8string().c_str()),
                                   DefaultDeleter<Module>{ allocator }, STLAllocator{ allocator } };
            if(!mod)
                context().getErrorHandler().raiseException("Failed to initialize module \"" + name + "\".",
                                                           PIPER_SOURCE_LOCATION());
            {
                std::unique_lock<std::recursive_mutex> guard{ mMutex };
                mModules.insert(makePair(name, mod));
                mHandles.emplace_back(lib);
                mContext.registerDelegatedObject(std::move(lib));
                mContext.registerDelegatedObject(std::move(mod));
            }
            //});
            return context().getScheduler().ready();
        }
    }
}  // namespace Piper

PIPER_API Piper::PiperContextOwner* piperCreateContext() {
    return new Piper::PiperContextImpl();
}
PIPER_API void piperDestroyContext(Piper::PiperContextOwner* context) {
    delete context;
}
