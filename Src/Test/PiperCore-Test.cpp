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

#include "../Interface/Infrastructure/Logger.hpp"
#include "../PiperContext.hpp"
#include <gtest/gtest.h>
#include <memory>

TEST(PiperCore, InitAndUninit) {
    struct ContextDeleter final {
        void operator()(Piper::PiperContextOwner* ptr) const {
            piperDestoryContext(ptr);
        }
    };
    std::unique_ptr<Piper::PiperContextOwner, ContextDeleter> context{ piperCreateContext() };
    context->getLogger().record(Piper::LogLevel::Info, "Hello World", PIPER_SOURCE_LOCATION());
    context.reset();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
