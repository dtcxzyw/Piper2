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
#include "../STL/Vector.hpp"
#include "TestEnvironment.hpp"
#include <atomic>

using namespace std::chrono_literals;

// TODO:change test layer
void generalConcurrencyTest(Piper::PiperContext& context) {
    auto a = context.getScheduler().value(1);
    ASSERT_TRUE(a.ready());
    ASSERT_EQ(a.get(), 1);
    auto b = context.getScheduler().value(2);
    ASSERT_TRUE(b.ready());
    ASSERT_EQ(b.get(), 2);
    std::atomic_size_t count{ 0 };
    auto c = context.getScheduler().spawn(
        [&count](const Piper::Future<int32_t>& x, const Piper::Future<int32_t>& y) {
            ++count;
            std::this_thread::sleep_for(1000ms);
            return x.get() + y.get();
        },
        a, b);
    ASSERT_FALSE(c.ready());
    context.getScheduler().waitAll();
    ASSERT_TRUE(c.ready());
    auto d = context.getScheduler().spawn([](const Piper::Future<int32_t>& x) { return x.get() * x.get(); }, c);
    ASSERT_EQ(d.get(), 9);
    auto e = context.getScheduler().spawn(
        [](const Piper::Future<int32_t>& x, const Piper::Future<int32_t>& y) { return x.get() + y.get(); }, c, c);
    ASSERT_EQ(e.get(), 6);
    ASSERT_EQ(count, 1);
    // exception

    // zero-copy+initialize once
    // compilation time
    /*
    {
        struct UncopyValue final {
            size_t value;
            explicit UncopyValue(size_t val) : value(val) {}
            UncopyValue(const UncopyValue& rhs) = delete;
            UncopyValue& operator=(const UncopyValue& rhs) = delete;
        };
        UncopyValue val{ 5 };
        auto trans = context.getScheduler().spawn([](Piper::Future<UncopyValue>&& x) { return std::move(x).get(); },
                                                  context.getScheduler().value(val));
        ASSERT_EQ(trans.get().value, 5);
    }
    */
    // runtime
    {
        struct CopyCount final {
            size_t copyCount;
            CopyCount() : copyCount(0) {}
            CopyCount(const CopyCount& rhs) : copyCount(rhs.copyCount + 1) {}
            CopyCount& operator=(const CopyCount&) {
                throw;
            }
            CopyCount(CopyCount&& rhs) = default;
            CopyCount& operator=(CopyCount&&) = default;
        };
        CopyCount val;
        auto trans = context.getScheduler().spawn([](Piper::Future<CopyCount>& x) { return std::move(x).get(); },
                                                  context.getScheduler().value(std::move(val)));
        ASSERT_EQ(trans.get().copyCount, 0);
    }
    // event
    // event+notify
}

TEST_F(PiperCoreEnvironment, Taskflow) {
    auto desc = Piper::makeSharedObject<Piper::Config>(*context);
    desc->at("Path").set("Infrastructure/Concurrency/Taskflow");
    desc->at("Name").set("Piper.Infrastructure.Concurrency.Taskflow");
    auto&& loader = context->getModuleLoader();
    const auto mod = loader.loadModule(desc, ".");
    auto inst = loader
                    .newInstance("Piper.Infrastructure.Concurrency.Taskflow.Scheduler",
                                 Piper::makeSharedObject<Piper::Config>(*context), mod)
                    .get();
    contextOwner->setScheduler(eastl::dynamic_shared_pointer_cast<Piper::Scheduler>(inst));
    generalConcurrencyTest(*context);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
