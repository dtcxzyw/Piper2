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
#include "../../STL/String.hpp"
#include "../../STL/StringView.hpp"
#include "../../STL/Vector.hpp"
#include "../Object.hpp"
#include "Concurrency.hpp"

namespace Piper {
    enum class FileType { File, Directory, NotExist };
    enum class Permission { Read = 1, Write = 2, Create = 4, Delete = 8 };

    class FileSystem : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(FileSystem, Object)
        //virtual void createFile(const StringView& path) = 0;
        virtual void removeFile(const StringView& path) = 0;
        virtual String findFile(const StringView& path, const Span<StringView>& searchDirs) = 0;

        virtual void createDir(const StringView& path) = 0;
        virtual void removeDir(const StringView& path) = 0;
        virtual String findDir(const StringView& path, const Span<StringView>& searchDirs) = 0;

        virtual bool exist(const StringView& path) = 0;
        virtual Permission permission(const StringView& path) = 0;
        //virtual size_t spaceUsage(const StringView& path) = 0;

        //virtual String prepareNativeFileGroup(const StringView& path) = 0;

        virtual ~FileSystem() = 0{}
    };
}  // namespace Piper
