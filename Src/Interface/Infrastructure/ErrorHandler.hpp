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
#include "../../STL/String.hpp"
#include "../Object.hpp"
#include "Logger.hpp"

namespace Piper {
    class ErrorHandler;

    class PIPER_API StageGuard final : private Unmovable {
    private:
        ErrorHandler& mHandler;

    public:
        explicit StageGuard(ErrorHandler& handler) noexcept : mHandler(handler) {}
        void switchTo(const String& stage, const SourceLocation& loc);
        void switchToStatic(const CString stage, const SourceLocation& loc);
        ~StageGuard() noexcept;
    };

    class ErrorHandler : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ErrorHandler, Object)
        virtual ~ErrorHandler() = default;

        // for runtime error
        virtual void raiseException(const StringView& message, const SourceLocation& loc) = 0;

        // for coding error
        // TODO:error information
        enum class CheckLevel {
            InterfaceArgument = 1,  // caller
            RightSideEffect = 2,    // callee
            InternalInvariant = 4   // self
        };
        virtual bool allowAssert(const CheckLevel level) = 0;
        virtual void assertFailed(const CheckLevel level, const CString expression, const SourceLocation& loc) = 0;
        virtual void processSignal(int signal) = 0;
        [[noreturn]] virtual void notImplemented(const SourceLocation& loc) = 0;

        // before abort
        // virtual void addFinalAction() = 0;

        // for tracing
        StageGuard enterStage(const String& stage, const SourceLocation& loc) {
            enterStageImpl(stage, loc);
            return StageGuard{ *this };
        }
        StageGuard enterStageStatic(const CString stage, const SourceLocation& loc) {
            // TODO:no allocation stroage
            enterStageImpl(String(stage, context().getAllocator()), loc);
            return StageGuard{ *this };
        }

    private:
        friend class StageGuard;
        virtual void exitStage() noexcept = 0;
        virtual void enterStageImpl(const String& stage, const SourceLocation& loc) = 0;
    };

#define PIPER_CHECK_IMPL(handler, level, expr)                           \
    do {                                                                 \
        if(handler.allow(level) && !(expr))                              \
            handler.assertFailed(level, #expr, PIPER_SOURCE_LOCATION()); \
    } while(false)

#define PIPER_CHECK_INTERFACE_ARGUMENT(handler, expr) PIPER_CHECK_IMPL(handler, ErrorHandler::CheckLevel::InterfaceArgument, expr)
#define PIPER_CHECK_RIGHT_SIDE_EFFECT(handler, expr) PIPER_CHECK_IMPL(handler, ErrorHandler::CheckLevel::RightSideEffect, expr)
#define PIPER_CHECK_INTERNAL_INVARIANT(handler, expr) PIPER_CHECK_IMPL(handler, ErrorHandler::CheckLevel::InternalInvariant, expr)
}  // namespace Piper
