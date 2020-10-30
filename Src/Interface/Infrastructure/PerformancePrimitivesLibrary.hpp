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
#include "Program.hpp"

namespace Piper {
    class FloatingPointLibrary : public Object {
    public:
        enum class Instruction { Float16, BFloat16, Float32, Float64, Float128, Common };
        PIPER_INTERFACE_CONSTRUCT(FloatingPointLibrary, Object)
        virtual Instruction instruction() const noexcept = 0;
        virtual size_t elementSize() const noexcept = 0;
        virtual size_t elementAlignment() const noexcept = 0;
        virtual String typeName() const = 0;
        virtual Future<SharedPtr<PITU>> generateLinkable(const SharedPtr<PITUManager>& manager) const = 0;
        virtual ~FloatingPointLibrary() = default;
    };

    class MathKernelLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(MathKernelLibrary, Object)
        virtual bool supportFPType(FloatingPointLibrary::Instruction fpType) const noexcept = 0;
        virtual Pair<Future<Vector<std::byte>>, CString> generateLinkable(const Span<CString>& acceptableFormat,
                                                                          FloatingPointLibrary::Instruction fpType,
                                                                          const StringView& fpName = {}) const = 0;
        virtual ~MathKernelLibrary() = default;
    };

    class TransformLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(TransformLibrary, Object)
        virtual Pair<Future<Vector<std::byte>>, CString> generateLinkable(const Span<CString>& acceptableFormat,
                                                                          FloatingPointLibrary::Instruction fpType,
                                                                          const StringView& fpName = {}) const = 0;
        virtual ~TransformLibrary() = default;
    };

    class GeometryLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(GeometryLibrary, Object)
        virtual Pair<Future<Vector<std::byte>>, CString> generateLinkable(const Span<CString>& acceptableFormat,
                                                                          FloatingPointLibrary::Instruction fpType,
                                                                          const StringView& fpName = {}) const = 0;
        virtual ~GeometryLibrary() = default;
    };

    /*
    class RandomLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(RandomLibrary, Object);
        virtual ~RandomLibrary() = default;
    };

    class FFTLibrary : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(FFTLibrary, Object);
        virtual ~FFTLibrary() = default;
    };
    */
}  // namespace Piper
