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

#include "../Interface/Infrastructure/Concurrency.hpp"
#include "../Interface/Infrastructure/Config.hpp"
#include "../Interface/Infrastructure/Logger.hpp"
#include "../Interface/Infrastructure/Module.hpp"
#include "../Interface/Infrastructure/PhysicalQuantitySIDesc.hpp"
#include "../STL/UniquePtr.hpp"
#include "TestEnvironment.hpp"

TEST(PiperCore, InitAndUninitTest) {
    Piper::UniquePtr<Piper::PiperContextOwner, ContextDeleter> context{ piperCreateContext() };
    context->getLogger().record(Piper::LogLevel::Info, "Hello,World!", PIPER_SOURCE_LOCATION());
    context.reset();
}

TEST_F(PiperCoreEnvironment, ConcurrencyTest) {
    auto&& scheduler = context->getScheduler();
    auto a = scheduler.value(1);
    a.wait();
    ASSERT_EQ(*a, 1);
    auto b = scheduler.value(2);
    b.wait();
    ASSERT_EQ(*b, 2);
    auto c = scheduler.spawn([](const Piper::Future<int>& x, const Piper::Future<int>& y) { return *x + *y; }, a, b);
    c.wait();
    ASSERT_EQ(*c, 3);
}

TEST_F(PiperCoreEnvironment, ConfigTest) {
    // integer
    auto config = Piper::makeSharedObject<Piper::Config>(*context, 1);
    ASSERT_EQ(config->type(), Piper::NodeType::SignedInteger);
    config = Piper::makeSharedObject<Piper::Config>(*context, 1U);
    ASSERT_EQ(config->type(), Piper::NodeType::UnsignedInteger);
    config = Piper::makeSharedObject<Piper::Config>(*context, 1LL);
    ASSERT_EQ(config->type(), Piper::NodeType::SignedInteger);
    config = Piper::makeSharedObject<Piper::Config>(*context, 1ULL);
    ASSERT_EQ(config->type(), Piper::NodeType::UnsignedInteger);
    // floating point
    config = Piper::makeSharedObject<Piper::Config>(*context, 1.0f);
    ASSERT_EQ(config->type(), Piper::NodeType::FloatingPoint);
    config = Piper::makeSharedObject<Piper::Config>(*context, 1.0);
    ASSERT_EQ(config->type(), Piper::NodeType::FloatingPoint);
    // string
    config = Piper::makeSharedObject<Piper::Config>(*context, Piper::String{ "Piper", context->getAllocator() });
    ASSERT_EQ(config->type(), Piper::NodeType::String);
    config = Piper::makeSharedObject<Piper::Config>(*context, "Piper");
    ASSERT_EQ(config->type(), Piper::NodeType::String);
    config = Piper::makeSharedObject<Piper::Config>(*context, Piper::StringView{ "Piper" });
    ASSERT_EQ(config->type(), Piper::NodeType::String);
    // boolean
    config = Piper::makeSharedObject<Piper::Config>(*context, true);
    ASSERT_EQ(config->type(), Piper::NodeType::Boolean);
    // null
    config = Piper::makeSharedObject<Piper::Config>(*context, Piper::MonoState{});
    ASSERT_EQ(config->type(), Piper::NodeType::Null);

    // object
    config->at("Name") = Piper::makeSharedObject<Piper::Config>(*context, "Piper");
    ASSERT_EQ(config->type(), Piper::NodeType::Object);
    ASSERT_EQ("Piper", config->at("Name")->get<Piper::String>());
    Piper::UMap<Piper::String, Piper::SharedPtr<Piper::Config>> map{ context->getAllocator() };
    map.insert(Piper::makePair(Piper::String("Name", context->getAllocator()), Piper::makeSharedObject<Piper::Config>(*context)));
    config = Piper::makeSharedObject<Piper::Config>(*context, std::move(map));
    ASSERT_EQ(config->type(), Piper::NodeType::Object);
    // array
    config = Piper::makeSharedObject<Piper::Config>(
        *context,
        Piper::Vector<Piper::SharedPtr<Piper::Config>>({ Piper::makeSharedObject<Piper::Config>(*context) },
                                                       context->getAllocator()));
    ASSERT_EQ(config->type(), Piper::NodeType::Array);
    ASSERT_EQ(config->viewAsArray()[0]->type(), Piper::NodeType::Null);
}

TEST_F(PiperCoreEnvironment, UnitManagerTest) {
    Piper::PhysicalQuantitySIDesc desc{};
    desc.m = 2, desc.kg = 1, desc.s = -3;
    auto&& unit = context->getUnitManager();
    ASSERT_EQ(Piper::StringView(unit.serialize(desc, false)), Piper::StringView("m^2*kg*s^-3"));
    unit.addTranslation(desc, "W");
    ASSERT_EQ(Piper::StringView(unit.serialize(desc, false)), Piper::StringView("W"));
    ASSERT_EQ(Piper::StringView(unit.serialize(desc, true)), Piper::StringView("m^2*kg*s^-3"));
    ASSERT_EQ(desc, unit.deserialize("W"));
    ASSERT_EQ(desc, unit.deserialize("m^2*kg*s^-3"));
    desc.s += 1;
    ASSERT_EQ(desc, unit.deserialize("W*s"));
}

TEST_F(PiperCoreEnvironment, ModuleLoaderTest) {
    auto&& loader = context->getModuleLoader();
    auto inst = loader.newInstance("Piper.Infrastructure.MemoryFS.MemoryFS", Piper::makeSharedObject<Piper::Config>(*context));
    inst.wait();
    auto object = std::move(*inst);
    ASSERT_EQ(context, &object->context());
    object.reset();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
