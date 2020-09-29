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
#include "STL/Vector.hpp"
#include <iostream>
// use builtin allocator
#include <cstdlib>
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
namespace std {
    void* aligned_alloc(std::size_t alignment, std::size_t size) {
        return _aligned_malloc(size, alignment);
    }
}  // namespace std
void freeMemory(void* ptr) {
    _aligned_free(ptr);
}
#else
void freeMemory(void* ptr) {
    free(ptr);
}
#endif

namespace EA {
    namespace StdC {
        PIPER_API int Vsnprintf(char* EA_RESTRICT pDestination, size_t n, const char* EA_RESTRICT pFormat, va_list arguments) {
            return vsnprintf(pDestination, n, pFormat, arguments);
        }
    }  // namespace StdC
}  // namespace EA

namespace Piper {

    void* STLAllocator::allocate(size_t n, int flags) {
        if(flags != 0)
            throw;
        return reinterpret_cast<void*>(mAllocator->alloc(n));
    }
    void* STLAllocator::allocate(size_t n, size_t alignment, size_t offset, int flags) {
        throw;
    }
    void STLAllocator::deallocate(void* p, size_t n) {
        mAllocator->free(reinterpret_cast<Ptr>(p));
    }

    void ObjectDeleter::operator()(Object* obj) {
        obj->~Object();
        mAllocator.free(reinterpret_cast<Ptr>(obj));
    }

    const UMap<String, SharedObject<Config>>& Config::viewAsObject() const {
        return Piper::get<UMap<String, SharedObject<Config>>>(mValue);
    }

    const Vector<SharedObject<Config>>& Config::viewAsArray() const {
        return Piper::get<Vector<SharedObject<Config>>>(mValue);
    };

    const SharedObject<Config>& Config::operator()(const StringView& key) const {
        return viewAsObject().find(String(key, context().getAllocator()))->second;
    }
    const SharedObject<Config>& Config::operator[](Index index) const {
        return viewAsArray()[index];
    }

    class DefaultAllocator final : public Allocator {
    public:
        explicit DefaultAllocator(PiperContext& view) : Allocator(view) {}
        ContextHandle getContextHandle() const {
            return 0;
        }
        Ptr alloc(const size_t size, const size_t align) override {
            static_assert(sizeof(Ptr) == sizeof(void*));
            return reinterpret_cast<Ptr>(std::aligned_alloc(align, size));
        }
        void free(Ptr ptr) override {
            freeMemory(reinterpret_cast<void*>(ptr));
        }
    };

    class UnitManagerImpl final : public UnitManager {
    private:
        struct Hasher final {
            size_t operator()(const PhysicalQuantitySIDesc& desc) const {
                // FNV1-a
                uint64_t res = 14695981039346656037ULL;
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&desc);
                const uint8_t* end = reinterpret_cast<const uint8_t*>(&desc) + sizeof(desc);
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
#define Unit(x)                                            \
    if(desc.x) {                                           \
        res += #x;                                         \
        res += '^';                                        \
        res += toString(context().getAllocator(), desc.x); \
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
            auto iter = mD2S.find(desc);
            return iter == mD2S.cend() ? serializeSI(desc) : iter->second;
        }

    public:
        explicit UnitManagerImpl(PiperContext& context)
            : UnitManager(context), mD2S(context.getAllocator()), mS2D(context.getAllocator()) {}
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
            PhysicalQuantitySIDesc desc;
            memset(&desc, 0, sizeof(desc));
            throw;
            return desc;
        }
    };

#ifdef PIPER_WIN32
    static void raiseWin32Error() {
        // throw GetLastError();
        throw;
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
        UMap<String, Pair<DLLHandle, SharedObject<Module>>> mModules;
        // USet<String> mClasses; TODO:Cache
        std::shared_mutex mMutex;

    public:
        explicit ModuleLoaderImpl(PiperContext& context) : ModuleLoader(context), mModules(context.getAllocator()) {}
        Future<void> loadModule(const SharedObject<Config>& packageDesc, const StringView& descPath) override {
            auto&& info = packageDesc->viewAsObject();
            auto iter = info.find(String("Path", context().getAllocator()));
            if(iter == info.cend())
                throw;
            auto path = String(descPath, context().getAllocator()) + "/" + iter->second->get<String>();
            iter = info.find(String("Name", context().getAllocator()));
            if(iter == info.cend())
                throw;
            auto name = iter->second->get<String>();
            // TODO:filesystem
            // TODO:multi load
            return context().getScheduler().spawn([this, name, path] {
                // TODO:checksum/dependencies
                Owner<void*> handle = reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
                if(handle == nullptr)
                    raiseWin32Error();
                DLLHandle lib{ handle };
                using InitFunc = Module* (*)(PiperContext & context);
                auto func = reinterpret_cast<InitFunc>(lib.getFunctionAddress("initModule"));
                // TODO:ABI
                auto mod = SharedObject<Module>{ func(context()), ObjectDeleter{ context().getAllocator() },
                                                 STLAllocator{ context().getAllocator() } };
                if(!mod)
                    throw;
                String name{ name, context().getAllocator() };
                {
                    std::unique_lock<std::shared_mutex> guard{ mMutex };
                    mModules.insert(makePair(name, makePair(std::move(lib), mod)));
                }
            });
        };
        Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config) override {
            // TODO:lazy load
            auto pos = classID.find_last_of('.');
            auto iter = mModules.find(String(classID.substr(0, pos), context().getAllocator()));
            if(iter == mModules.cend())
                throw;
            return iter->second.second->newInstance(classID.substr(pos + 1), config);
        }
        ~ModuleLoaderImpl() {
            // destruction order
            throw;
        }
    };

    class LoggerImpl final : public Logger {
    private:
        CString level2Str(const LogLevel level) const noexcept {
            switch(level) {
                case LogLevel::Info:
                    return "INFO";
                    break;
                case LogLevel::Warning:
                    return "WARNING";
                    break;
                case LogLevel::Error:
                    return "ERROR";
                    break;
                case LogLevel::Fatal:
                    return "FATAL";
                    break;
                case LogLevel::Debug:
                    return "DEBUG";
                    break;
                default:
                    return "UNKNOWN";
                    break;
            }
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(LoggerImpl, Logger)
        bool allow(const LogLevel level) const noexcept override {
            return true;
        }
        void record(const LogLevel level, const StringView& message, const SourceLocation& sourceLocation) noexcept override {
            std::cerr << "[" << level2Str(level) << "]" << message.data() << std::endl;
        }
        void flush() noexcept override {
            std::cerr.flush();
        }
    };

    class FutureStorage final : public FutureImpl {
    private:
        void* mPtr;

        void* alloc(PiperContext& context, const size_t size) {
            if(size) {
                Ptr ret{};
                context.getAllocator().alloc(size, ret);
                return reinterpret_cast<void*>(ret);
            }
            return nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size, const void* value)
            : FutureImpl(context), mPtr(alloc(context, size)) {
            if(value)
                memcpy(mPtr, value, size);
        }
        bool ready() const noexcept override {
            return true;
        }
        const void* get() const override {
            return mPtr;
        }
    };

    class SchedulerImpl final : public Scheduler {
    public:
        PIPER_INTERFACE_CONSTRUCT(SchedulerImpl, Scheduler)

        void spawnImpl(const Function<void, void*>& func, const Span<const SharedObject<FutureImpl>>& dependencies,
                       const SharedObject<FutureImpl>& res) override {
            for(auto&& dep : dependencies)
                dep->get();
            func(const_cast<void*>(res->get()));
        }
        SharedObject<FutureImpl> newFutureImpl(const size_t size, const void* value) override {
            return makeSharedPtr<FutureStorage>(context().getAllocator(), context(), size, value);
        }
        void waitAll() noexcept override {}
    };

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
        PiperContextImpl()
            : mDefaultAllocator(*this), mAllocator(&mDefaultAllocator), mModuleLoader(*this),
              mLifeTimeRecorder(STLAllocator{ getAllocator() }), mUnitManager(*this) {
            setLogger(makeSharedPtr<LoggerImpl>(getAllocator(), *this));
            setScheduler(makeSharedPtr<SchedulerImpl>(getAllocator(), *this));
            setFileSystem(makeSharedPtr<FileSystemImpl>(getAllocator(), *this));
        }
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
            mScheduler->waitAll();
            while(!mLifeTimeRecorder.empty())
                mLifeTimeRecorder.pop();
        }
    };
}  // namespace Piper

PIPER_API Piper::PiperContextOwner* piperCreateContext() {
    return new Piper::PiperContextImpl();
}
PIPER_API void piperDestoryContext(Piper::PiperContextOwner* context) {
    delete context;
}
