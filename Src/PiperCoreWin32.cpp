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
#include "Interface/Infrastructure/IO.hpp"
#include "PiperContext.hpp"
#include "STL/DynamicArray.hpp"
#include "STL/Pair.hpp"
#include "STL/SharedPtr.hpp"
#include "STL/StringView.hpp"
#include "STL/UniquePtr.hpp"

#pragma warning(push,0)
#define NOMINMAX
#include <Windows.h>
#include <fileapi.h>
#pragma warning(pop)

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

void* allocMemory(const size_t alignment, const size_t size) {
    return _aligned_malloc(size, alignment);
}
void freeMemory(void* ptr) noexcept {
    _aligned_free(ptr);
}

[[noreturn]] static void raiseWin32Error(Piper::PiperContext& context, Piper::CString func, const Piper::SourceLocation& loc) {
    const auto code = GetLastError();
    char buf[1024];
    Piper::String err = Piper::String{ "Failed to call function ", context.getAllocator() } + func +
        "\nError Code:" + Piper::toString(context.getAllocator(), static_cast<uint64_t>(code)) + "\nReason:";
    // TODO:use en-US
    // https://docs.microsoft.com/zh-cn/windows/win32/api/winbase/nf-winbase-formatmessagea
    if(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, code, 0, buf, sizeof(buf), nullptr))
        err += buf;
    else
        err += "Unknown";

    context.getErrorHandler().raiseException(err, loc);
}

void freeModule(Piper::PiperContext& context, void* handle) {
    if(!FreeLibrary(static_cast<HMODULE>(handle)))
        raiseWin32Error(context, "FreeLibrary", PIPER_SOURCE_LOCATION());
}
void* loadModule(Piper::PiperContext& context, const fs::path& path) {
    const auto res = LoadLibraryExW(fs::absolute(path).wstring().c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!res)
        raiseWin32Error(context, "LoadLibraryExW", PIPER_SOURCE_LOCATION());
    return res;
}
Piper::CString getModuleExtension() {
    return ".dll";
}

static size_t getFileSize(void* handle) {
    DWORD high;
    size_t size = GetFileSize(handle, &high);
    size += static_cast<size_t>(high) << 32;
    return size;
}
void* getModuleSymbol(Piper::PiperContext& context, void* handle, const Piper::CString symbol) {
    const auto res = GetProcAddress(static_cast<HMODULE>(handle), symbol);
    if(res)
        return static_cast<void*>(res);
    raiseWin32Error(context, "GetProcAddress", PIPER_SOURCE_LOCATION());
}

namespace Piper {
    struct IOPayload final {
        OVERLAPPED overlapped;
        Allocator* allocator;
        FutureImpl* future;
        std::atomic_size_t remainCount;
        std::atomic_size_t remainSize;
        SharedPtr<Pair<bool, DynamicArray<std::byte>>> data;
        SharedPtr<void> file;
    };

    struct ReadFuture final : public FutureImpl {
        SharedPtr<Pair<bool, DynamicArray<std::byte>>> data;

        ReadFuture(PiperContext& context, const STLAllocator& allocator, const size_t size)
            : FutureImpl(context), data(makeSharedPtr<Pair<bool, DynamicArray<std::byte>>>(
                                       allocator, false, DynamicArray<std::byte>{ size, allocator })) {}

        [[nodiscard]] bool fastReady() const noexcept override {
            return data->first;
        }
        bool ready() const noexcept override {
            return data->first;
        }
        void wait() const override {
            while(!data->first)
                std::this_thread::yield();
        }
        const void* storage() const override {
            return &data->second;
        }
        bool supportNotify() const noexcept override {
            return true;
        }
    };

    class StreamImpl final : public Piper::Stream {
    private:
        SharedPtr<void> mHandle;
        size_t mSize;
        std::mutex mMutex;
        static constexpr size_t readUnit = 4095U << 20U;

    public:
        StreamImpl(PiperContext& context, SharedPtr<void>&& handle)
            : Stream(context), mHandle(std::move(handle)), mSize(getFileSize(mHandle.get())) {}
        size_t size() const noexcept override {
            return mSize;
        }
        Future<DynamicArray<std::byte>> read(const size_t offset, const size_t size) override {
            auto future = makeSharedObject<ReadFuture>(context(), context().getAllocator(), size);
            auto& allocator = context().getAllocator();
            auto payload = reinterpret_cast<IOPayload*>(allocator.alloc(sizeof(IOPayload)));
            new(payload) IOPayload();
            memset(&payload->overlapped, 0, sizeof(payload->overlapped));
            // set payload->overlapped.Offset?
            payload->future = future.get();
            payload->allocator = &allocator;
            payload->remainCount = (size + readUnit - 1) / readUnit;
            payload->data = future->data;
            payload->file = mHandle;

            auto low = static_cast<LONG>(offset), high = static_cast<LONG>(offset >> 32);
            {
                std::lock_guard<std::mutex> guard{ mMutex };
                size_t readCount = 0;
                // TODO:ReadFileScatter
                SetFilePointer(mHandle.get(), low, &high, FILE_BEGIN);
                while(readCount != size) {
                    DWORD needRead = static_cast<DWORD>(std::min(size - readCount, readUnit));
                    auto res = ReadFile(mHandle.get(), future->data->second.data() + readCount, needRead, nullptr,
                                        reinterpret_cast<LPOVERLAPPED>(payload));
                    if(!res)
                        raiseWin32Error(context(), "ReadFile", PIPER_SOURCE_LOCATION());
                    readCount += needRead;
                }
            }
            return Future<DynamicArray<std::byte>>{ future };
        }
        Future<void> write(const size_t offset, const Future<DynamicArray<std::byte>>& data) override {
            throw;
            /*
            return context().getScheduler().spawn(
                [offset, context = &context(), handle = mHandle, this](const Future<DynamicArray<std::byte>>& span) {
                    auto& data = span.get();
                    auto size = data.size();
                    auto low = static_cast<LONG>(offset), high = static_cast<LONG>(offset >> 32);
                    {
                        std::lock_guard guard{ mMutex };
                        size_t writeCount = 0;
                        SetFilePointer(handle.get(), low, &high, FILE_BEGIN);
                        while(writeCount != size) {
                            DWORD needWrite = static_cast<DWORD>((size - writeCount) % readUnit), write = 0;
                            auto res = WriteFile(handle.get(), data.data(), needWrite, &write, nullptr);
                            if(!res || needWrite != write)
                                raiseWin32Error();
                            writeCount += needWrite;
                        }
                        mSize = std::max(mSize, offset + size);
                    }
                },
                data);
                */
        }
    };

    struct HandleCloser {
        void operator()(void* handle) const {
            CloseHandle(handle);
        }
    };

    class MappedSpanImpl final : public MappedSpan {
    private:
        void* mPtr;
        Span<std::byte> mSpan;

    public:
        MappedSpanImpl(PiperContext& context, void* ptr, const Span<std::byte>& span)
            : MappedSpan(context), mPtr(ptr), mSpan(span) {}

        Span<std::byte> get() const noexcept override {
            return mSpan;
        }

        virtual ~MappedSpanImpl() noexcept {
            if(mPtr && !UnmapViewOfFile(mPtr))
                raiseWin32Error(context(), "UnmapViewOfFile", PIPER_SOURCE_LOCATION());
        }
    };

    static size_t getMemoryAlignmentImpl() {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return info.dwAllocationGranularity;
    }

    static size_t getMemoryAlignment() {
        static const size_t align = getMemoryAlignmentImpl();
        return align;
    }

    class MappedMemoryImpl final : public MappedMemory {
    private:
        SharedPtr<void> mFileHandle;
        UniquePtr<void, HandleCloser> mMapHandle;
        const size_t mSize;
        const FileAccessMode mAccess;

    public:
        MappedMemoryImpl(PiperContext& context, SharedPtr<void>&& handle, const FileAccessMode access, const size_t maxSize)
            : MappedMemory(context), mFileHandle(std::move(handle)), mSize(maxSize ? maxSize : getFileSize(mFileHandle.get())),
              mAccess(access) {
            if(mSize != 0) {
                auto mapHandle = CreateFileMappingW(mFileHandle.get(), nullptr,
                                                    access == FileAccessMode::Read ? PAGE_READONLY : PAGE_READWRITE,
                                                    static_cast<DWORD>(maxSize >> 32), static_cast<DWORD>(maxSize), nullptr);
                if(mapHandle == INVALID_HANDLE_VALUE)
                    raiseWin32Error(context, "CreateFileMappingW", PIPER_SOURCE_LOCATION());
                mMapHandle.reset(mapHandle);
            }
        }
        size_t size() const noexcept override {
            return mSize;
        }
        size_t alignment() const noexcept override {
            return getMemoryAlignment();
        }
        SharedPtr<MappedSpan> map(const size_t offset, const size_t size) const override {
            if(mMapHandle) {
                size_t end = offset + size;
                size_t rem = offset % getMemoryAlignment();
                size_t roffset = offset - rem;
                auto ptr = reinterpret_cast<std::byte*>(
                    MapViewOfFile(mMapHandle.get(), mAccess == FileAccessMode::Read ? FILE_MAP_READ : FILE_MAP_WRITE,
                                  static_cast<DWORD>(roffset >> 32), static_cast<DWORD>(roffset), end - roffset));
                if(ptr == nullptr)
                    throw;
                auto base = ptr + offset - roffset;
                return makeSharedObject<MappedSpanImpl>(context(), ptr, Span<std::byte>(base, base + size));
            }
            return makeSharedObject<MappedSpanImpl>(context(), nullptr, Span<std::byte>{});
        }
    };

    // TODO:root path
    class FileSystemImpl final : public FileSystem {
    private:
        DynamicArray<std::thread> mWorkers;
        UniquePtr<void, HandleCloser> mIOCP;
        static constexpr auto exitFlag = std::numeric_limits<size_t>::max();

        auto openFile(const StringView& path, const FileAccessMode access, const FileCacheHint hint, bool async) {
            const auto nativePath = fs::u8path(path.cbegin(), path.cend());
            const auto handle = CreateFileW(
                nativePath.generic_wstring().c_str(),
                (access == FileAccessMode::Read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE),
                (access == FileAccessMode::Write ? 0 : FILE_SHARE_READ), nullptr,
                (access == FileAccessMode::Read ? OPEN_EXISTING : (fs::exists(nativePath) ? TRUNCATE_EXISTING : CREATE_NEW)),
                (hint == FileCacheHint::Random ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN) |
                    (async ? FILE_FLAG_OVERLAPPED : 0),
                nullptr);
            if(handle == INVALID_HANDLE_VALUE)
                raiseWin32Error(context(), "CreateFileW", PIPER_SOURCE_LOCATION());

            // TODO:FILE_FLAG_WRITE_THROUGH
            if(async) {
                if(CreateIoCompletionPort(handle, mIOCP.get(), 0, static_cast<DWORD>(mWorkers.size())) != mIOCP.get())
                    raiseWin32Error(context(), "CreateIoCompletePort", PIPER_SOURCE_LOCATION());
                // TODO:SetFileIoOverlappedRange
                // if(!SetFileIoOverlappedRange(handle, 0, getFileSize(handle)))
                // raiseWin32Error();
            }

            return SharedPtr<void>(handle, HandleCloser{}, STLAllocator{ context().getAllocator() });
        }

    public:
        explicit FileSystemImpl(PiperContext& context) : FileSystem(context), mWorkers(context.getAllocator()) {
            const auto poolSize = 2 * std::thread::hardware_concurrency();
            // TODO:shared with socket
            const auto handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, poolSize);
            if(!handle)
                raiseWin32Error(context, "CreateIoCompletionPort", PIPER_SOURCE_LOCATION());
            mIOCP.reset(handle);
            auto worker = [this, &context] {
                while(true) {
                    DWORD count;
                    ULONG_PTR key;  // TODO:IO Type for file/socket
                    LPOVERLAPPED overlapped;
                    if(!GetQueuedCompletionStatus(mIOCP.get(), &count, &key, &overlapped, INFINITE))
                        raiseWin32Error(context, "GetQueuedCompletionStatus", PIPER_SOURCE_LOCATION());
                    if(key == exitFlag)
                        return;
                    auto* payload = reinterpret_cast<IOPayload*>(overlapped);
                    payload->remainSize -= count;
                    if((--payload->remainCount) == 0) {
                        if(payload->remainSize != 0)
                            throw;
                        payload->data->first = true;
                        // TODO: assert:future has been destroyed
                        if(payload->data.use_count() == 1)
                            throw;
                        context.notify(payload->future);
                        payload->data.reset();
                        payload->file.reset();
                        payload->allocator->free(reinterpret_cast<Ptr>(payload));
                    }
                }
            };
            for(size_t i = 0; i < poolSize; ++i)
                mWorkers.emplace_back(std::thread(worker));
        }

        virtual ~FileSystemImpl() noexcept {
            for(Index i = 0; i < mWorkers.size(); ++i)
                if(!PostQueuedCompletionStatus(mIOCP.get(), 0, exitFlag, nullptr))
                    raiseWin32Error(context(), "PostQueuedCompletionStatus", PIPER_SOURCE_LOCATION());
            for(auto&& t : mWorkers)
                t.join();
        }
        SharedPtr<Stream> openFileStream(const StringView& path, const FileAccessMode access, const FileCacheHint hint) override {
            return makeSharedObject<StreamImpl>(context(), openFile(path, access, hint, true));
        }
        SharedPtr<MappedMemory> mapFile(const StringView& path, const FileAccessMode access, const FileCacheHint hint,
                                        const size_t maxSize) override {
            return makeSharedObject<MappedMemoryImpl>(context(), openFile(path, access, hint, false), access, maxSize);
        }
        void removeFile(const StringView& path) override {
            fs::remove(path.data());
        }
        void createDir(const StringView& path) override {
            fs::create_directory(path.data());
        }
        void removeDir(const StringView& path) override {
            fs::remove_all(path.data());
        }
        bool exist(const StringView& path) override {
            return fs::exists(path.data());
        }
    };

}  // namespace Piper

void nativeFileSystem(Piper::PiperContextOwner& context) {
    context.setFileSystem(Piper::makeSharedObject<Piper::FileSystemImpl>(context));
}
