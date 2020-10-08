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

#include "../../STL/String.hpp"
#include "../../STL/Vector.hpp"
#include "../ContextResource.hpp"
#include "Allocator.hpp"
#include "Concurrency.hpp"

namespace Piper {
    struct Dim3 final {
        uint32_t x, y, z;
    };

    class RunnableProgram;
    class LinkableProgram;

    using CommandQueue = uint64_t;

    class Accelerator : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(Accelerator, ContextResource);
        virtual ~Accelerator() = default;
        virtual const Vector<CString>& getSupportedLinkableFormat() const = 0;
        virtual Future<SharedObject<RunnableProgram>> compileKernel(const Vector<Future<Vector<std::byte>>>& linkable,
                                                                    const String& entry /*resource access*/);
        virtual void runKernel(const Dim3& dim, const Future<SharedObject<RunnableProgram>>& entry) = 0;
        virtual void input() = 0;
        virtual void output() = 0;
        virtual void apply(Function<void, CommandQueue>) = 0;
    };
}  // namespace Piper
