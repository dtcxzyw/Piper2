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

#include "../../STL/UniquePtr.hpp"
#include "../Object.hpp"

namespace Piper {
    class Acceleration;
    class RTProgram;
    class Tracer;

    class Geometry : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(Geometry, Object);
        virtual ~Geometry() = default;
        virtual UniquePtr<Acceleration> build(Tracer& tracer) const = 0;
        virtual UniquePtr<RTProgram> compile(Tracer& tracer) const = 0;
    };
}  // namespace Piper
