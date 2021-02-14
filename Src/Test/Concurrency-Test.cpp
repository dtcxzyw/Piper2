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

#include "../Interface/Infrastructure/Allocator.hpp"
#include "../Interface/Infrastructure/Config.hpp"
#include "../Interface/Infrastructure/Module.hpp"
#include "../STL/List.hpp"
#include "TestEnvironment.hpp"

#include <atomic>
#include <random>

using namespace std::chrono_literals;

static constexpr auto magic = 142342;

void valueAndReady(Piper::Scheduler& scheduler) {
    {
        auto val = scheduler.value(magic);
        ASSERT_TRUE(val.ready());
        ASSERT_EQ(val.getUnsafe(), magic);
    }
    {
        auto hold = std::make_unique<int32_t>(magic);
        auto val = scheduler.value(std::move(hold));
        ASSERT_FALSE(hold.get());
        ASSERT_TRUE(val.getUnsafe());
        ASSERT_EQ(*val.getUnsafe(), magic);
    }
    {
        const auto ready = scheduler.ready();
        ASSERT_TRUE(ready.ready());
    }
}
void commonSpawn(Piper::Scheduler& scheduler) {
    // T
    auto a = scheduler.value(1);
    ASSERT_TRUE(a.ready());
    ASSERT_EQ(a.getUnsafe(), 1);
    auto b = scheduler.value(2);
    ASSERT_TRUE(b.ready());
    ASSERT_EQ(b.getUnsafe(), 2);
    auto c = scheduler.spawn([](const int32_t x, const int32_t y) { return x + y; }, a, b);
    c.wait();
    ASSERT_TRUE(c.ready());
    ASSERT_EQ(c.getUnsafe(), 3);
    // void
    auto call1 = false, call2 = false, call3 = false;
    auto foo1 = scheduler.spawn([&call1] {
        std::this_thread::sleep_for(10ms);
        call1 = true;
    });
    auto foo2 = scheduler.spawn(
        [&call2](Piper::PlaceHolder) {
            std::this_thread::sleep_for(10ms);
            call2 = true;
        },
        foo1);
    const auto foo3 = scheduler.spawn(
        [&call3](Piper::PlaceHolder) {
            std::this_thread::sleep_for(10ms);
            call3 = true;
        },
        foo2);
    foo3.wait();
    ASSERT_TRUE(foo1.ready());
    ASSERT_TRUE(call1);
    ASSERT_TRUE(foo2.ready());
    ASSERT_TRUE(call2);
    ASSERT_TRUE(foo3.ready());
    ASSERT_TRUE(call3);
    // reference
}
void nonBlocking(Piper::Scheduler& scheduler) {
    auto x = scheduler.spawn([] {
        std::this_thread::sleep_for(10ms);
        return magic;
    });
    ASSERT_FALSE(x.ready());
    x.wait();
    ASSERT_TRUE(x.ready());
    ASSERT_EQ(x.getUnsafe(), magic);
}
void callOnce(Piper::Scheduler& scheduler) {
    std::atomic_size_t count{ 0 };
    auto c = scheduler.spawn(
        [&count](const int32_t x, const int32_t y) {
            ++count;
            std::this_thread::sleep_for(10ms);
            return x + y;
        },
        1, 2);
    c.wait();
    ASSERT_TRUE(c.ready());
    ASSERT_EQ(c.getUnsafe(), 3);
    ASSERT_EQ(count, 1);
}
void zeroCopy(Piper::Scheduler& scheduler) {

    // compilation time
    {
        struct UncopyableValue final {
            size_t value;
            explicit UncopyableValue(const size_t val) : value(val) {}
            UncopyableValue(const UncopyableValue& rhs) = delete;
            UncopyableValue& operator=(const UncopyableValue& rhs) = delete;
            UncopyableValue(UncopyableValue&& rhs) = default;
            UncopyableValue& operator=(UncopyableValue&& rhs) = delete;
        };
        auto trans = scheduler.spawn([](UncopyableValue x) { return std::move(x); }, scheduler.value(UncopyableValue{ magic }));
        trans.wait();
        ASSERT_EQ(trans.getUnsafe().value, magic);
    }
    // runtime
    {
        struct CopyCount final {
            size_t copyCount;
            CopyCount() : copyCount(0) {}
            CopyCount(const CopyCount& rhs) : copyCount(rhs.copyCount + 1) {}
            void operator=(const CopyCount&) const {
                FAIL();
            }
            CopyCount(CopyCount&& rhs) = default;
            CopyCount& operator=(CopyCount&&) = default;
        };
        auto trans = scheduler.spawn([](CopyCount x) { return x; }, scheduler.value(CopyCount{}));
        trans.wait();
        ASSERT_EQ(trans.getUnsafe().copyCount, 0);
    }
}
void ownership(Piper::Scheduler& scheduler) {
    auto src = std::make_unique<int>(magic);
    auto trans = scheduler.spawn([](std::unique_ptr<int32_t> x) { return std::move(x); }, scheduler.value(std::move(src)));
    ASSERT_FALSE(src.get());
    trans.wait();
    ASSERT_TRUE(trans.getUnsafe().get());
    ASSERT_EQ(*trans.getUnsafe().get(), magic);
}
void wrap(Piper::Scheduler& scheduler) {
    // T
    Piper::DynamicArray<Piper::Future<size_t>> data(scheduler.context().getAllocator());
    for(size_t i = 0; i < 10; ++i)
        data.emplace_back(scheduler.spawn(
            [](size_t i) {
                std::this_thread::sleep_for(10ms);
                return i + 1;
            },
            i));
    auto wrap = scheduler.wrap(data);
    wrap.wait();
    ASSERT_EQ(wrap.getUnsafe().size(), 10);
    for(size_t i = 0; i < 10; ++i)
        ASSERT_EQ(wrap.getUnsafe()[i], i + 1);
    // void
    Piper::DynamicArray<Piper::Future<void>> events(scheduler.context().getAllocator());
}
void returnFuture(Piper::Scheduler& scheduler) {
    auto future = scheduler.spawn([ctx = &scheduler.context()] { return ctx->getScheduler().value(magic); });
    future.wait();
    ASSERT_EQ(future.getUnsafe(), magic);
}
void parallel(Piper::Scheduler& scheduler) {
    Piper::DynamicArray<int32_t> res(100, scheduler.context().getAllocator());
    scheduler.parallelFor(static_cast<uint32_t>(res.size()), [&res](uint32_t i) { res[i] = i + 10; }).wait();
    for(size_t i = 0; i < res.size(); ++i)
        ASSERT_EQ(res[i], i + 10);
}
void notify(Piper::Scheduler& scheduler) {
    struct Event final : public Piper::FutureImpl {
        bool readyFlag;
        int32_t mPayload;
        explicit Event(Piper::PiperContext& context) : Piper::FutureImpl(context), readyFlag(false), mPayload(magic) {}

        [[nodiscard]] bool fastReady() const noexcept override {
            return readyFlag;
        }

        [[nodiscard]] bool ready() const noexcept override {
            return readyFlag;
        }
        void wait() const override {
            while(!readyFlag)
                std::this_thread::yield();
        }

        [[nodiscard]] const void* storage() const override {
            return &mPayload;
        }

        [[nodiscard]] bool supportNotify() const noexcept override {
            return true;
        }
    };
    auto eventHandle = Piper::makeSharedObject<Event>(scheduler.context());
    auto event = Piper::Future<int32_t>{ eventHandle };
    ASSERT_FALSE(event.ready());
    auto future = scheduler.spawn([](int32_t x) { return x * 2; }, event);
    const auto yetAnotherThread = std::async([eventHandle, ctx = &scheduler.context()] {
        std::this_thread::sleep_for(1000ms);
        eventHandle->readyFlag = true;
        ctx->notify(eventHandle.get());
    });
    ASSERT_FALSE(future.ready());
    future.wait();
    ASSERT_TRUE(yetAnotherThread.wait_for(0ms) == std::future_status::ready);
    ASSERT_TRUE(future.ready());
    ASSERT_EQ(future.getUnsafe(), 2 * magic);
}
void futureCallProxy(Piper::Scheduler& scheduler) {
    class Instance final : public Piper::Object {
    private:
        int32_t mData;

    public:
        explicit Instance(Piper::PiperContext& context, const int32_t data) : Piper::Object(context), mData(0) {
            std::this_thread::sleep_for(100ms);
            mData = data;
        }
        Piper::Future<int32_t> apply(Piper::Scheduler* scheduler) const {
            std::this_thread::sleep_for(10ms);
            return scheduler->value(mData);
        }
    };
    auto inst = scheduler.spawn([](Piper::PiperContext* context) { return Piper::makeSharedObject<Instance>(*context, magic); },
                                &scheduler.context());
    auto future = PIPER_FUTURE_CALL(inst, apply)(&scheduler);
    ASSERT_FALSE(inst.ready());
    ASSERT_FALSE(future.ready());
    future.wait();
    ASSERT_TRUE(inst.ready());
    ASSERT_TRUE(future.ready());
    ASSERT_EQ(future.getUnsafe(), magic);
}
void futureMemberAccess(Piper::Scheduler& scheduler) {
    struct Instance final : public Piper::Object {
        int32_t data;

        explicit Instance(Piper::PiperContext& context, const int32_t input) : Piper::Object(context) {
            std::this_thread::sleep_for(100ms);
            data = input;
        }
    };
    auto inst = scheduler.spawn([](Piper::PiperContext* context) { return Piper::makeSharedObject<Instance>(*context, magic); },
                                &scheduler.context());
    auto future = PIPER_FUTURE_ACCESS(inst, data);
    ASSERT_FALSE(inst.ready());
    ASSERT_FALSE(future.ready());
    future.wait();
    ASSERT_TRUE(inst.ready());
    ASSERT_TRUE(future.ready());
    ASSERT_EQ(future.getUnsafe(), magic);
}
void dynamicParallelism(Piper::Scheduler& scheduler) {
    auto future = scheduler.spawn([ctx = &scheduler.context()] {
        std::this_thread::sleep_for(100ms);
        return ctx->getScheduler().spawn([ctx] {
            std::this_thread::sleep_for(100ms);
            return ctx->getScheduler().spawn([ctx] {
                std::this_thread::sleep_for(100ms);
                return ctx->getScheduler().value(magic);
            });
        });
    });
    future.wait();
    ASSERT_EQ(future.getUnsafe(), magic);
}

void multiTasks(Piper::Scheduler& scheduler) {
    std::mt19937_64 eng{ static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) };
    constexpr uint32_t tasks = 10000, count = 10000;
    const std::uniform_int_distribution<uint32_t> dis(0, count);
    Piper::List<Piper::Future<void>> futures{ scheduler.context().getAllocator() };
    std::atomic_uint64_t execCount{ 0 };
    std::atomic_uint64_t sum{ 0 };
    uint64_t standardCount = 0, standardSum = 0;
    for(uint32_t i = 0; i < tasks; ++i) {
        const auto cnt = dis(eng);
        const auto mul = dis(eng);
        standardCount += cnt;
        standardSum += static_cast<uint64_t>(mul) * static_cast<uint64_t>(cnt) * static_cast<uint64_t>(cnt - 1) / 2;
        futures.emplace_back(scheduler.parallelFor(cnt, [&, mul](const uint32_t idx) {
            std::this_thread::sleep_for(10ns);
            ++execCount;
            sum += static_cast<uint64_t>(mul) * static_cast<uint64_t>(idx);
        }));
    }
    while(!futures.empty()) {
        futures.remove_if([](Piper::Future<void>& future) { return future.ready(); });
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_EQ(static_cast<uint64_t>(execCount), standardCount);
    ASSERT_EQ(static_cast<uint64_t>(sum), standardSum);
}

#define TEST_ENVIRONMENT(NAME, CLASS_ID)                                                     \
    class NAME : public PiperCoreEnvironment {                                               \
    protected:                                                                               \
        void SetUp() override {                                                              \
            PiperCoreEnvironment::SetUp();                                                   \
            auto inst = context->getModuleLoader().newInstanceT<Piper::Scheduler>(CLASS_ID); \
            contextOwner->setScheduler(inst.getSync());                                      \
        }                                                                                    \
    };

#define TEST_CONTENT(NAME, FUNC)       \
    TEST_F(NAME, NAME##_##FUNC) {      \
        FUNC(context->getScheduler()); \
    }
#define TEST_MODULE(NAME, CLASS_ID)        \
    TEST_ENVIRONMENT(NAME, CLASS_ID)       \
    TEST_CONTENT(NAME, valueAndReady)      \
    TEST_CONTENT(NAME, commonSpawn)        \
    TEST_CONTENT(NAME, nonBlocking)        \
    TEST_CONTENT(NAME, callOnce)           \
    TEST_CONTENT(NAME, zeroCopy)           \
    TEST_CONTENT(NAME, ownership)          \
    TEST_CONTENT(NAME, wrap)               \
    TEST_CONTENT(NAME, returnFuture)       \
    TEST_CONTENT(NAME, parallel)           \
    TEST_CONTENT(NAME, notify)             \
    TEST_CONTENT(NAME, futureCallProxy)    \
    TEST_CONTENT(NAME, futureMemberAccess) \
    TEST_CONTENT(NAME, dynamicParallelism) \
    TEST_CONTENT(NAME, multiTasks)

TEST_MODULE(Taskflow, "Piper.Infrastructure.Taskflow.Scheduler")
TEST_MODULE(Squirrel, "Piper.Infrastructure.Squirrel.Scheduler")
#undef TEST_CONTENT
#undef TEST_MODULE
#undef TEST_ENVIRONMENT

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
