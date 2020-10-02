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
#include "../../STL/GSL.hpp"
#include "../../STL/StringView.hpp"
#include "../Object.hpp"

namespace Piper {

    enum class LogLevel { Info = 1, Warning = 2, Error = 4, Fatal = 8, Debug = 16 };

    struct SourceLocation final {
        CString file;
        CString func;
        int line;
    };

    #define PIPER_SOURCE_LOCATION() Piper::SourceLocation{__FILE__,__FUNCTION__,__LINE__}

    class Logger : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Logger, Object)
        virtual bool allow(const LogLevel level) const noexcept = 0;
        virtual void record(const LogLevel level, const StringView& message, const SourceLocation& sourceLocation) noexcept = 0;
        virtual void flush() noexcept = 0;
        //maxlevel
        // virtual void bindStream()=0;
        virtual ~Logger() = 0{}
    };

}  // namespace Piper
