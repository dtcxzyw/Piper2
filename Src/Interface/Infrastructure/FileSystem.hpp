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

#pragma once
#include "../../STL/StringView.hpp"
#include "../Object.hpp"
#include "IO.hpp"

namespace Piper {
    enum class FileType { File, Directory, NotExist };
    enum class FilePermission { Read = 1, Write = 2, Create = 4, Delete = 8 };
    enum class FileAccessMode { Read, Write };
    enum class FileCacheHint { Random, Sequential };

    class FileSystem : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(FileSystem, Object)
        // TODO:enumerate files
        virtual SharedObject<Stream> openFileStream(const StringView& path, const FileAccessMode access,
                                                    const FileCacheHint hint) = 0;
        virtual SharedObject<MappedMemory> mapFile(const StringView& path, const FileAccessMode access, const FileCacheHint hint,
                                                   const size_t maxSize = 0) = 0;
        virtual void removeFile(const StringView& path) = 0;

        virtual void createDir(const StringView& path) = 0;
        virtual void removeDir(const StringView& path) = 0;

        virtual bool exist(const StringView& path) = 0;
        // virtual FilePermission permission(const StringView& path) = 0;  // TODO:User Group
        // virtual size_t spaceUsage(const StringView& path) = 0;

        // virtual String prepareNativeFileGroup(const StringView& path) = 0;

        virtual ~FileSystem() = default;
    };
}  // namespace Piper
