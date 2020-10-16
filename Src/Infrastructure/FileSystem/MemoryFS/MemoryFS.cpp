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
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <new>

namespace Piper {
    struct FileTag final {};
    struct DirTag final {};

    class StorageUnit final : public Object {
    private:
        Variant<UMap<String, SharedPtr<StorageUnit>>, Vector<std::byte>> mData;
        auto& viewAsDir() {
            if(isFile())
                throw;
            return get<UMap<String, SharedPtr<StorageUnit>>>(mData);
        }

    public:
        StorageUnit(PiperContext& context, FileTag) : Object(context), mData(Vector<std::byte>{ context.getAllocator() }) {}
        StorageUnit(PiperContext& context, DirTag)
            : Object(context), mData(UMap<String, SharedPtr<StorageUnit>>{ context.getAllocator() }) {}
        bool isFile() const noexcept {
            return mData.index() == 1;
        }
        auto& viewAsFile() {
            if(!isFile())
                throw;
            return get<Vector<std::byte>>(mData);
        }
        void remove(StorageUnit* ptr) {
            auto& dir = viewAsDir();
            for(auto iter = dir.cbegin(); iter != dir.cend(); ++iter)
                if(iter->second.get() == ptr) {
                    dir.erase(iter);
                    return;
                }
            throw;
        }
        void insert(const String& key, const SharedPtr<StorageUnit>& unit) {
            auto& dir = viewAsDir();
            if(dir.count(key))
                throw;
            dir.insert(makePair(key, unit));
        }
        StorageUnit* locate(const String& key) {
            auto& dir = viewAsDir();
            auto iter = dir.find(key);
            if(iter == dir.cend())
                return nullptr;
            return iter->second.get();
        }
    };

    class StreamImpl final : public Stream {
    private:
        Vector<std::byte>& mData;
        FileAccessMode mAccess;

    public:
        StreamImpl(PiperContext& context, Vector<std::byte>& data, const FileAccessMode access)
            : Stream(context), mData(data), mAccess(access) {}
        size_t size() const noexcept override {
            return mData.size();
        }
        Future<Vector<std::byte>> read(const size_t offset, const size_t size) override {
            if(mAccess != FileAccessMode::Read)
                throw;
            return context().getScheduler().value(Vector<std::byte>(mData.cbegin() + offset, mData.cbegin() + offset + size));
        }
        Future<void> write(const size_t offset, const Future<Vector<std::byte>>& data) override {
            if(mAccess != FileAccessMode::Write)
                throw;
            return context().getScheduler().spawn(
                [offset, this](const Future<Vector<std::byte>>& val) {
                    eastl::copy(val->cbegin(), val->cend(), mData.begin());
                    // TODO:extend/thread safety
                },
                data);
        }
    };

    class MappedSpanImpl final : public MappedSpan {
    private:
        Span<std::byte> mData;

    public:
        MappedSpanImpl(PiperContext& context, const Span<std::byte>& data) : MappedSpan(context), mData(data) {}
        Span<std::byte> get() const noexcept override {
            return mData;
        }
    };

    class MappedMemoryImpl final : public MappedMemory {
    private:
        Vector<std::byte>& mData;
        const size_t mMaxSize;

    public:
        MappedMemoryImpl(PiperContext& context, Vector<std::byte>& data, const size_t maxSize)
            : MappedMemory(context), mData(data), mMaxSize(maxSize) {}
        size_t size() const noexcept override {
            return mMaxSize == std::numeric_limits<size_t>::max() ? mData.size() : mMaxSize;
        }
        size_t alignment() const noexcept override {
            return 1 << 16;
        }
        SharedPtr<MappedSpan> map(const size_t offset, const size_t size) const override {
            if(this->size() <= offset + size)
                throw;
            if(mData.size() <= offset + size)
                mData.resize(offset + size);
            return makeSharedObject<MappedSpanImpl>(context(),
                                                    Span<std::byte>{ mData.begin() + offset, mData.begin() + offset + size });
        }
    };

    class MemoryFS final : public FileSystem {
    private:
        StorageUnit mRoot;
        auto normalize(const StringView& path) {
            Vector<StringView> stack;
            Index lastPos = 0;
            for(Index i = 0; i <= path.size(); ++i) {
                if(i == path.size() || path[i] == '/') {
                    if(lastPos != i) {
                        auto part = path.substr(lastPos, i - lastPos);
                        if(part == "..")
                            stack.pop_back();
                        else if(!(part == "."))
                            stack.push_back(part);
                    }
                    lastPos = i + 1;
                }
            }
            return stack;
        }
        StorageUnit* locate(const Vector<StringView>& path, StorageUnit** parent) {
            StorageUnit* res = &mRoot;
            for(auto&& part : path) {
                if(res) {
                    auto ptr = res->locate(String{ part, context().getAllocator() });
                    if(ptr) {
                        if(parent)
                            *parent = res;
                        res = ptr;
                    } else {
                        if(parent) {
                            // store parent
                            *parent = res;
                            res = nullptr;
                        } else
                            return nullptr;
                    }
                } else {
                    return nullptr;
                }
            }
            return res;
        }
        StringView name(const Vector<StringView>& path) {
            return path.back();
        }

        Vector<std::byte>& openFile(const StringView& path, const FileAccessMode access) {
            auto np = normalize(path);
            StorageUnit* parent = nullptr;
            auto unit = locate(np, &parent);
            if(access == FileAccessMode::Read) {
                if(!unit)
                    throw;
                return unit->viewAsFile();
            } else {
                if(unit)
                    throw;
                auto newFile = makeSharedObject<StorageUnit>(context(), FileTag{});
                parent->insert(String{ name(np), context().getAllocator() }, newFile);
                return newFile->viewAsFile();
            }
        }

    public:
        explicit MemoryFS(PiperContext& context) : FileSystem(context), mRoot(context, DirTag{}) {}
        // TODO:memory protection
        SharedPtr<Stream> openFileStream(const StringView& path, const FileAccessMode access, const FileCacheHint) {
            return makeSharedObject<StreamImpl>(context(), openFile(path, access), access);
        }
        SharedPtr<MappedMemory> mapFile(const StringView& path, const FileAccessMode access, const FileCacheHint,
                                           const size_t maxSize) override {
            return makeSharedObject<MappedMemoryImpl>(
                context(), openFile(path, access), access == FileAccessMode::Read ? std::numeric_limits<size_t>::max() : maxSize);
        }
        void removeFile(const StringView& path) override {
            auto np = normalize(path);
            StorageUnit* parent = nullptr;
            StorageUnit* unit = locate(np, &parent);
            if(!parent || !unit || !unit->isFile())
                throw;
            parent->remove(unit);
        }
        void createDir(const StringView& path) override {
            auto np = normalize(path);
            StorageUnit* parent = nullptr;
            StorageUnit* unit = locate(np, &parent);
            if(!parent || parent->isFile() || unit)
                throw;
            parent->insert(String{ name(np), context().getAllocator() }, makeSharedObject<StorageUnit>(context(), DirTag{}));
        }
        void removeDir(const StringView& path) override {
            auto np = normalize(path);
            StorageUnit* parent = nullptr;
            StorageUnit* unit = locate(np, &parent);
            if(!parent || !unit || unit->isFile())
                throw;
            parent->remove(unit);
        }

        bool exist(const StringView& path) override {
            auto np = normalize(path);
            return locate(np, nullptr);
        }
    };
    class ModuleImpl final : public Module {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                                 const Future<void>& module) override {
            if(classID == "MemoryFS") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<MemoryFS>(context())));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
