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

#pragma once
#include "../../PiperContext.hpp"
#include "../../STL/GSL.hpp"
#include "../../STL/String.hpp"
#include "../../STL/Variant.hpp"
#include "../Object.hpp"
#include "Logger.hpp"

namespace Piper {
    // TODO:cross-thread stage chain
    class PIPER_API StageGuard final : private Unmovable {
    private:
        ErrorHandler& mHandler;

    public:
        explicit StageGuard(ErrorHandler& handler) noexcept : mHandler(handler) {}
        void next(Variant<CString, String> stage, const SourceLocation& loc);
        ~StageGuard() noexcept;
    };

    class ErrorHandler : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ErrorHandler, Object);

        virtual void setupGlobalErrorHandler() = 0;
        virtual void setupThreadErrorHandler() = 0;

        // for runtime error
        [[noreturn]] virtual void raiseException(const StringView& message, const SourceLocation& loc) = 0;
        [[noreturn]] virtual void unresolvedClassID(const StringView& classID, const SourceLocation& loc) = 0;

        // for coding error
        // TODO:error information
        enum class CheckLevel {
            InterfaceArgument = 1,  // caller
            RightSideEffect = 2,    // callee
            InternalInvariant = 4   // self
        };
        virtual bool allowAssert(CheckLevel level) = 0;
        virtual void assertFailed(CheckLevel level, CString expression, const SourceLocation& loc) = 0;
        virtual void processSignal(int signal) = 0;
        [[noreturn]] virtual void notImplemented(const SourceLocation& loc) = 0;
        [[noreturn]] virtual void notSupported(const SourceLocation& loc) = 0;
        [[noreturn]] virtual void unreachable(const SourceLocation& loc) = 0;
        virtual String backTrace() = 0;

        // before abort
        // virtual void addFinalAction() = 0;

        // for tracing
        // TODO: additional information: error buffer
        StageGuard enterStage(Variant<CString, String> stage, const SourceLocation& loc) {
            enterStageImpl(std::move(stage), loc);
            return StageGuard{ *this };
        }

    private:
        friend class StageGuard;
        virtual void exitStage() noexcept = 0;
        virtual void enterStageImpl(Variant<CString, String> stage, const SourceLocation& loc) = 0;
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
