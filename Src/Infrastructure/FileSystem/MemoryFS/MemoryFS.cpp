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
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../STL/Pair.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <new>

namespace Piper {
    struct FileTag final {};
    struct DirTag final {};

    class StorageUnit final : public Object {
    private:
        Variant<UMap<String, SharedPtr<StorageUnit>>, Binary> mData;
        auto& viewAsDir() {
            if(isFile())
                context().getErrorHandler().assertFailed(ErrorHandler::CheckLevel::InternalInvariant, "Bad filesystem node type.",
                                                         PIPER_SOURCE_LOCATION());
            return get<UMap<String, SharedPtr<StorageUnit>>>(mData);
        }

    public:
        StorageUnit(PiperContext& context, FileTag) : Object(context), mData(Binary{ context.getAllocator() }) {}
        StorageUnit(PiperContext& context, DirTag)
            : Object(context), mData(UMap<String, SharedPtr<StorageUnit>>{ context.getAllocator() }) {}
        [[nodiscard]] bool isFile() const noexcept {
            return mData.index() == 1;
        }
        auto& viewAsFile() {
            if(!isFile())
                context().getErrorHandler().assertFailed(ErrorHandler::CheckLevel::InternalInvariant, "Bad filesystem node type.",
                                                         PIPER_SOURCE_LOCATION());
            return get<Binary>(mData);
        }
        void remove(StorageUnit* ptr) {
            auto& dir = viewAsDir();
            for(auto iter = dir.cbegin(); iter != dir.cend(); ++iter)
                if(iter->second.get() == ptr) {
                    dir.erase(iter);
                    return;
                }
            context().getErrorHandler().assertFailed(ErrorHandler::CheckLevel::InternalInvariant,
                                                     "Filesystem has been corrupted.", PIPER_SOURCE_LOCATION());
        }
        void insert(const String& key, const SharedPtr<StorageUnit>& unit) {
            auto& dir = viewAsDir();
            if(dir.count(key))
                context().getErrorHandler().raiseException("Invalid path \"" + key + "\".", PIPER_SOURCE_LOCATION());
            dir.insert(makePair(key, unit));
        }
        StorageUnit* locate(const String& key) {
            auto& dir = viewAsDir();
            const auto iter = dir.find(key);
            if(iter == dir.cend())
                return nullptr;
            return iter->second.get();
        }
    };

    class StreamImpl final : public Stream {
    private:
        Binary& mData;
        FileAccessMode mAccess;

    public:
        StreamImpl(PiperContext& context, Binary& data, const FileAccessMode access)
            : Stream(context), mData(data), mAccess(access) {}
        [[nodiscard]] size_t size() const noexcept override {
            return mData.size();
        }
        Future<Binary> read(const size_t offset, const size_t size) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return Future<Binary>{ nullptr };
        }
        Future<void> write(const size_t offset, const Future<Binary>& data) override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return Future<void>{ nullptr };
        }
    };

    class MappedSpanImpl final : public MappedSpan {
    private:
        Span<std::byte> mData;

    public:
        MappedSpanImpl(PiperContext& context, const Span<std::byte>& data) : MappedSpan(context), mData(data) {}
        [[nodiscard]] Span<std::byte> get() const noexcept override {
            return mData;
        }
    };

    class MappedMemoryImpl final : public MappedMemory {
    private:
        Binary& mData;
        const size_t mMaxSize;

    public:
        MappedMemoryImpl(PiperContext& context, Binary& data, const size_t maxSize)
            : MappedMemory(context), mData(data), mMaxSize(maxSize) {}
        [[nodiscard]] size_t size() const noexcept override {
            return mMaxSize == std::numeric_limits<size_t>::max() ? mData.size() : mMaxSize;
        }
        [[nodiscard]] size_t alignment() const noexcept override {
            return 1 << 16;
        }
        [[nodiscard]] SharedPtr<MappedSpan> map(const size_t offset, const size_t size) const override {
            if(this->size() <= offset + size)
                context().getErrorHandler().raiseException("File mapping is out of bound.", PIPER_SOURCE_LOCATION());
            if(mData.size() <= offset + size)
                mData.resize(offset + size);
            return makeSharedObject<MappedSpanImpl>(context(),
                                                    Span<std::byte>{ mData.begin() + offset, mData.begin() + offset + size });
        }
    };

    class MemoryFS final : public FileSystem {
    private:
        StorageUnit mRoot;

        static auto normalize(const StringView& path) {
            DynamicArray<StringView> stack;
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
        StorageUnit* locate(const DynamicArray<StringView>& path, StorageUnit** parent) {
            StorageUnit* res = &mRoot;
            for(auto&& part : path) {
                if(res) {
                    auto* ptr = res->locate(String{ part, context().getAllocator() });
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
        [[nodiscard]] static StringView name(const DynamicArray<StringView>& path) {
            return path.back();
        }

        Binary& openFile(const StringView& path, const FileAccessMode access) {
            const auto np = normalize(path);
            StorageUnit* parent = nullptr;
            auto* unit = locate(np, &parent);
            if(access == FileAccessMode::Read) {
                if(!unit)
                    context().getErrorHandler().raiseException("Invalid file path.", PIPER_SOURCE_LOCATION());
                return unit->viewAsFile();
            } else {
                if(unit)
                    context().getErrorHandler().raiseException("This file has existed.", PIPER_SOURCE_LOCATION());
                auto newFile = makeSharedObject<StorageUnit>(context(), FileTag{});
                parent->insert(String{ name(np), context().getAllocator() }, newFile);
                return newFile->viewAsFile();
            }
        }

    public:
        explicit MemoryFS(PiperContext& context) : FileSystem(context), mRoot(context, DirTag{}) {}
        // TODO:memory protection
        SharedPtr<Stream> openFileStream(const StringView& path, const FileAccessMode access, const FileCacheHint) override {
            return makeSharedObject<StreamImpl>(context(), openFile(path, access), access);
        }
        SharedPtr<MappedMemory> mapFile(const StringView& path, const FileAccessMode access, const FileCacheHint,
                                        const size_t maxSize) override {
            return makeSharedObject<MappedMemoryImpl>(
                context(), openFile(path, access), access == FileAccessMode::Read ? std::numeric_limits<size_t>::max() : maxSize);
        }
        void removeFile(const StringView& path) override {
            const auto np = normalize(path);
            StorageUnit* parent = nullptr;
            auto* const unit = locate(np, &parent);
            if(!parent || !unit || !unit->isFile())
                context().getErrorHandler().raiseException("Invalid file path.", PIPER_SOURCE_LOCATION());
            parent->remove(unit);
        }
        void createDir(const StringView& path) override {
            const auto np = normalize(path);
            StorageUnit* parent = nullptr;
            auto* const unit = locate(np, &parent);
            if(!parent || parent->isFile() || unit)
                context().getErrorHandler().raiseException("This directory/file has existed.", PIPER_SOURCE_LOCATION());
            parent->insert(String{ name(np), context().getAllocator() }, makeSharedObject<StorageUnit>(context(), DirTag{}));
        }
        void removeDir(const StringView& path) override {
            const auto np = normalize(path);
            StorageUnit* parent = nullptr;
            auto* const unit = locate(np, &parent);
            if(!parent || !unit || unit->isFile())
                context().getErrorHandler().raiseException("Invalid directory path.", PIPER_SOURCE_LOCATION());
            parent->remove(unit);
        }

        bool exist(const StringView& path) override {
            const auto np = normalize(path);
            return locate(np, nullptr);
        }
    };
    class ModuleImpl final : public Module {
    public:
        ModuleImpl(PiperContext& context, const char*) : Module(context) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "MemoryFS") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<MemoryFS>(context())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
