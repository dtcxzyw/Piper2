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
#include <filesystem>
#include <mutex>
#include <shared_mutex>

#ifdef PIPER_WIN32
#define NOMAXMIN
#include <Windows.h>
#include <errhandlingapi.h>
#include <libloaderapi.h>
#endif

namespace fs = std::filesystem;

#ifdef PIPER_WIN32
void* allocMemory(std::size_t alignment, std::size_t size) {
    return _aligned_malloc(size, alignment);
}
void freeMemory(void* ptr) {
    _aligned_free(ptr);
}
#else
void* allocMemory(std::size_t alignment, std::size_t size) {
    return std::aligned_malloc(alignment, size);
}
void freeMemory(void* ptr) {
    free(ptr);
}
#endif

PIPER_API int EA::StdC::Vsnprintf(char* EA_RESTRICT pDestination, size_t n, const char* EA_RESTRICT pFormat, va_list arguments) {
    return vsnprintf(pDestination, n, pFormat, arguments);
}

namespace Piper {
    void* STLAllocator::allocate(size_t n, int flags) {
        return reinterpret_cast<void*>(mAllocator->alloc(n));
    }
    void* STLAllocator::allocate(size_t n, size_t alignment, size_t offset, int flags) {
        if(offset != 0)
            throw;
        return reinterpret_cast<void*>(mAllocator->alloc(n, alignment));
    }
    void STLAllocator::deallocate(void* p, size_t) {
        mAllocator->free(reinterpret_cast<Ptr>(p));
    }

    void ObjectDeleter::operator()(Object* obj) {
        obj->~Object();
        mAllocator.free(reinterpret_cast<Ptr>(obj));
    }

    class DefaultAllocator final : public Allocator {
    public:
        explicit DefaultAllocator(PiperContext& view) : Allocator(view) {}
        ContextHandle getContextHandle() const {
            return 0;
        }
        Ptr alloc(const size_t size, const size_t align) override {
            static_assert(sizeof(Ptr) == sizeof(void*));
            return reinterpret_cast<Ptr>(allocMemory(align, size));
        }
        void free(const Ptr ptr) override {
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

#ifdef PIPER_WIN32
    [[noreturn]] static void raiseWin32Error() {
        throw std::runtime_error(std::to_string(GetLastError()));
    }
#endif

    class DLLHandle final {
    private:
        struct HandleCloser {
            void operator()(void* handle) const {
#ifdef PIPER_WIN32
                if(!FreeLibrary(reinterpret_cast<HMODULE>(handle)))
                    raiseWin32Error();
#endif
            }
        };
        UniquePtr<void, HandleCloser> mHandle;

    public:
        explicit DLLHandle(void* handle) : mHandle(handle) {}
        void* getFunctionAddress(const StringView& symbol) const {
#ifdef PIPER_WIN32
            auto res = GetProcAddress(reinterpret_cast<HMODULE>(mHandle.get()), symbol.data());
            if(res)
                return res;
            raiseWin32Error();
#endif
        }
    };

    class ModuleLoaderImpl final : public ModuleLoader {
    private:
        UMap<String, SharedObject<Module>> mModules;
        Stack<DLLHandle> mHandles;
        // USet<String> mClasses; TODO:Cache
        std::shared_mutex mMutex;

    public:
        explicit ModuleLoaderImpl(PiperContext& context)
            : ModuleLoader(context), mModules(context.getAllocator()), mHandles(STLAllocator{ context.getAllocator() }) {}
        Future<void> loadModule(const SharedObject<Config>& packageDesc, const StringView& descPath) override {
            auto&& info = packageDesc->viewAsObject();
            auto iter = info.find(String("Path", context().getAllocator()));
            if(iter == info.cend())
                throw;
            auto base = String(descPath, context().getAllocator()) + "/";
            auto path = iter->second->get<String>();
            iter = info.find(String("Name", context().getAllocator()));
            if(iter == info.cend())
                throw;
            auto name = iter->second->get<String>();
            iter = info.find("Dependencies");
            Vector<String> deps(context().getAllocator());
            if(iter != info.cend()) {
                for(auto&& dep : iter->second->viewAsArray())
                    deps.push_back(dep->get<String>());
            }
            // TODO:filesystem
            // TODO:multi load
            return context().getScheduler().spawn([this, base, name, path, deps] {
            // TODO:checksum/utf-8
#ifdef PIPER_WIN32
                constexpr auto extension = ".dll";
#endif  // PIPER_WIN32
                for(auto&& dep : deps) {
#ifdef PIPER_WIN32
                    Owner<void*> handle = reinterpret_cast<void*>(LoadLibraryA((base + dep + extension).c_str()));
                    if(handle == nullptr)
                        raiseWin32Error();
#endif  // PIPER_WIN32
                    {
                        std::unique_lock<std::shared_mutex> guard{ mMutex };
                        mHandles.push(DLLHandle{ handle });
                    }
                }
#ifdef PIPER_WIN32
                Owner<void*> handle = reinterpret_cast<void*>(LoadLibraryA((base + path + extension).c_str()));
                if(handle == nullptr)
                    raiseWin32Error();
#endif PIPER_WIN32
                DLLHandle lib{ handle };
                using InitFunc = Module* (*)(PiperContext & context, Allocator & allocator);
                auto func = reinterpret_cast<InitFunc>(lib.getFunctionAddress("initModule"));
                // TODO:ABI
                auto& allocator = context().getAllocator();
                auto mod =
                    SharedObject<Module>{ func(context(), allocator), ObjectDeleter{ allocator }, STLAllocator{ allocator } };
                if(!mod)
                    throw;
                {
                    std::unique_lock<std::shared_mutex> guard{ mMutex };
                    mModules.insert(makePair(name, mod));
                    mHandles.push(std::move(lib));
                }
            });
        };
        Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
                                                 const Future<void>& module) override {
            std::shared_lock<std::shared_mutex> guard{ mMutex };
            // TODO:lazy load
            const auto pos = classID.find_last_of('.');
            const auto iter = mModules.find(String(classID.substr(0, pos), context().getAllocator()));
            if(iter == mModules.cend())
                throw;
            return iter->second->newInstance(classID.substr(pos + 1), config, module);
        }
        ~ModuleLoaderImpl() {
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
        ContextHandle mHandle;

        void* alloc(PiperContext& context, const size_t size) {
            if(size)
                return reinterpret_cast<void*>(context.getAllocator().alloc(size));
            return nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size, const ContextHandle handle)
            : FutureImpl(context), mPtr(alloc(context, size)), mHandle(handle) {}
        bool ready() const noexcept override {
            return true;
        }
        void* storage() const override {
            return mPtr;
        }
        void wait() const override {}
        ContextHandle getContextHandle() const override {
            return mHandle;
        }
    };

    class SchedulerImpl final : public Scheduler {
    public:
        PIPER_INTERFACE_CONSTRUCT(SchedulerImpl, Scheduler)

        void spawnImpl(const Function<void>& func, const Span<const SharedObject<FutureImpl>>& dependencies,
                       const SharedObject<FutureImpl>& res) override {
            for(auto&& dep : dependencies)
                dep->wait();
            func();
        }
        SharedObject<FutureImpl> newFutureImpl(const size_t size, const bool) override {
            return makeSharedPtr<FutureStorage>(context().getAllocator(), context(), size, getContextHandle());
        }
        void waitAll() noexcept override {}
        ContextHandle getContextHandle() const override {
            static char mark;
            return reinterpret_cast<ContextHandle>(&mark);
        }
    };

    // TODO:root
    class FileSystemImpl final : public FileSystem {
    public:
        PIPER_INTERFACE_CONSTRUCT(FileSystemImpl, FileSystem)
        // void createFile(const StringView& path) override {}
        void removeFile(const StringView& path) override {
            fs::remove(path.data());
        }
        String findFile(const StringView& path, const Span<StringView>& searchDirs) override {
            throw;
        }
        void createDir(const StringView& path) override {
            fs::create_directory(path.data());
        }
        void removeDir(const StringView& path) override {
            fs::remove_all(path.data());
        }
        String findDir(const StringView& path, const Span<StringView>& searchDirs) override {
            throw;
        }
        bool exist(const StringView& path) override {
            return fs::exists(path.data());
        }
        Permission permission(const StringView& path) override {
            throw;
        }
    };

    class PiperContextImpl final : public PiperContextOwner {
    private:
        DefaultAllocator mDefaultAllocator;
        Allocator* mAllocator;
        SharedObject<Logger> mLogger;
        ModuleLoaderImpl mModuleLoader;
        SharedObject<Scheduler> mScheduler;
        SharedObject<FileSystem> mFileSystem;
        SharedObject<Allocator> mUserAllocator;
        UnitManagerImpl mUnitManager;

        Stack<SharedObject<Object>> mLifeTimeRecorder;

    public:
        PiperContextImpl();

        Logger& getLogger() noexcept {
            return *mLogger;
        }
        ModuleLoader& getModuleLoader() noexcept {
            return mModuleLoader;
        }
        Scheduler& getScheduler() noexcept {
            return *mScheduler;
        }
        FileSystem& getFileSystem() noexcept {
            return *mFileSystem;
        }
        Allocator& getAllocator() noexcept {
            return *mAllocator;
        }
        UnitManager& getUnitManager() noexcept {
            return mUnitManager;
        }

        void setLogger(const SharedObject<Logger>& logger) noexcept {
            mLifeTimeRecorder.push(logger);
            mLogger = logger;
        }
        void setScheduler(const SharedObject<Scheduler>& scheduler) noexcept {
            mLifeTimeRecorder.push(scheduler);
            mScheduler = scheduler;
        }
        void setFileSystem(const SharedObject<FileSystem>& filesystem) noexcept {
            mLifeTimeRecorder.push(filesystem);
            mFileSystem = filesystem;
        }
        void setAllocator(const SharedObject<Allocator>& allocator) noexcept {
            mLifeTimeRecorder.push(allocator);
            mUserAllocator = allocator;
            mAllocator = mUserAllocator.get();
        }
        ~PiperContextImpl() {
            mLogger->record(LogLevel::Info, "Destroy Piper context", PIPER_SOURCE_LOCATION());
            mLogger->flush();
            mScheduler->waitAll();
            while(!mLifeTimeRecorder.empty())
                mLifeTimeRecorder.pop();
        }
    };

    PiperContextImpl::PiperContextImpl()
        : mDefaultAllocator(*this), mAllocator(&mDefaultAllocator), mModuleLoader(*this), mUnitManager(*this),
          mLifeTimeRecorder(STLAllocator{ getAllocator() }) {
        setLogger(makeSharedPtr<LoggerImpl>(getAllocator(), *this));
        mLogger->record(LogLevel::Info, "Create Piper context", PIPER_SOURCE_LOCATION());
        setScheduler(makeSharedPtr<SchedulerImpl>(getAllocator(), *this));
        setFileSystem(makeSharedPtr<FileSystemImpl>(getAllocator(), *this));
    }
}  // namespace Piper

PIPER_API Piper::PiperContextOwner* piperCreateContext() {
    return new Piper::PiperContextImpl();
}
PIPER_API void piperDestoryContext(Piper::PiperContextOwner* context) {
    delete context;
}
