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
#include "../Object.hpp"
#include "Logger.hpp"

namespace Piper {
    class ErrorHandler;

    class PIPER_API StageGuard final : private Unmovable {
    private:
        ErrorHandler& mHandler;

    public:
        explicit StageGuard(ErrorHandler& handler) noexcept : mHandler(handler) {}
        ~StageGuard() noexcept;
    };

    class ErrorHandler : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ErrorHandler, Object)
        virtual ~ErrorHandler() = default;

        // for coding error
        enum class CheckLevel {
            InterfaceArgument = 1,  // caller
            RightSideEffect = 2,    // callee
            InternalInvariant = 4   // self
        };
        virtual bool allow(const CheckLevel level) = 0;
        virtual void assertFailed(const CheckLevel level, const CString expression, const SourceLocation& loc) = 0;
        virtual void processSignal(int signal) = 0;

        // for recoverable error
        // virtual void addRecoverSolution() = 0;
        // virtual void tryRecover() = 0;

        // before abort
        virtual void addFinalAction() = 0;
        virtual void beforeAbort() noexcept = 0;

        // for tracing
        // TODO:Coroutine
        virtual StageGuard enterStage(const CString stage) = 0;

    private:
        friend class StageGuard;
        virtual void exitStage() noexcept = 0;
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
