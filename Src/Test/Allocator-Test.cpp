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

#include "../Interface/Infrastructure/Allocator.hpp"
#include "../Interface/Infrastructure/Config.hpp"
#include "../Interface/Infrastructure/Module.hpp"
#include "TestEnvironment.hpp"
#include <algorithm>

// TODO:change test layer
void generalAllocatorTest(Piper::PiperContext& context) {
    // aligned alloc
    auto ptr = context.getAllocator().alloc(1 << 20, 1 << 10);
    ASSERT_EQ(ptr & 1023, 0);
    context.getAllocator().free(ptr);
    // STL Container
    Piper::Vector<size_t> sum(context.getAllocator());
    for(Piper::Index i = 0; i < static_cast<Piper::Index>(100); ++i)
        sum.push_back(i);
    auto res = std::accumulate(sum.cbegin(), sum.cend(), static_cast<size_t>(0));
    ASSERT_EQ(res, static_cast<size_t>(4950));
    sum.clear();
    sum.shrink_to_fit();
    // Object
    auto object = Piper::makeSharedObject<Piper::Config>(context);
    ASSERT_EQ(&object->context(), &context);
    object.reset();
}

TEST_F(PiperCoreEnvironment, Jemalloc) {
    auto inst =
        context->getModuleLoader()
            .newInstance("Piper.Infrastructure.JemallocAllocator.Allocator", Piper::makeSharedObject<Piper::Config>(*context))
            .get();
    contextOwner->setAllocator(eastl::dynamic_shared_pointer_cast<Piper::Allocator>(inst));
    generalAllocatorTest(*context);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
