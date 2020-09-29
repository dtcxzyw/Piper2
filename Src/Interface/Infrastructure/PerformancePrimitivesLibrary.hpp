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
#include "Program.hpp"

namespace Piper {
    class FloatingPointLibrary : public Object {
    public:
        enum class Flag { TraceException = 1, TraceError = 2, HighestSupportedPrecise = 4 };
        enum class Instruction { Float16, Float32, Float64, Common };
        PIPER_INTERFACE_CONSTRUCT(FloatingPointLibrary, Object)
        virtual void traceError(bool trace) = 0;
        virtual void setFlag(Flag flag) = 0;
        virtual bool haveFlag() const noexcept = 0;
        virtual Instruction instruction() const noexcept = 0;
        virtual size_t elementSize() const noexcept = 0;
        virtual size_t elementAlign() const noexcept = 0;
        virtual ~FloatingPointLibrary() = 0{}
    };
    class MathKernelLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MathKernelLibrary, Object)
        virtual ~MathKernelLibrary() = 0{}
    };
    class TransformLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(TransformLibrary, Object);
        virtual ~TransformLibrary() = 0{}
    };
    class GeometryLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(GeometryLibrary, Object);
        virtual ~GeometryLibrary() = 0{}
    };
    class RandomLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RandomLibrary, Object);
        virtual ~RandomLibrary() = 0{}
    };
    class FFTLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(FFTLibrary, Object);
        virtual ~FFTLibrary() = 0{}
    };
    class MemoryOperationLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MemoryOperationLibrary, Object)
        virtual ~MemoryOperationLibrary() = 0{}
    };
}  // namespace Piper
