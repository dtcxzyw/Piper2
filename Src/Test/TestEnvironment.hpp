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
#include "../PiperContext.hpp"
#include "../STL/UniquePtr.hpp"
#include <gtest/gtest.h>

struct ContextDeleter {
    void operator()(Piper::PiperContextOwner* ptr) const {
        piperDestroyContext(ptr);
    }
};

class PiperCoreEnvironment : public testing::Test {
protected:
    Piper::UniquePtr<Piper::PiperContextOwner, ContextDeleter> contextOwner;
    Piper::PiperContext* context = nullptr;
    void SetUp() override {
        contextOwner.reset(piperCreateContext());
        context = contextOwner.get();
        const auto base = Piper::String{ ".", context->getAllocator() };
        auto name = Piper::makeSharedObject<Piper::Config>(*context, "Piper.Infrastructure.NlohmannJson");
        auto path = Piper::makeSharedObject<Piper::Config>(*context, "Infrastructure/Config/NlohmannJson/NlohmannJson");
        Piper::UMap<Piper::String, Piper::SharedPtr<Piper::Config>> desc{ context->getAllocator() };
        desc.insert(Piper::makePair(Piper::String{ "Name", context->getAllocator() }, name));
        desc.insert(Piper::makePair(Piper::String{ "Path", context->getAllocator() }, path));
        auto mod = context->getModuleLoader().loadModule(Piper::makeSharedObject<Piper::Config>(*context, std::move(desc)), base);
        auto parser = context->getModuleLoader().newInstanceT<Piper::ConfigSerializer>(
            Piper::String{ "Piper.Infrastructure.NlohmannJson.JsonSerializer", context->getAllocator() }, nullptr, mod);

        const auto modules = parser.getSync()->deserialize(Piper::String{ "Module.json", context->getAllocator() });

        for(auto&& modDesc : modules->viewAsArray())
            context->getModuleLoader().addModuleDescription(modDesc, base);
    }

    void TearDown() override {
        context = nullptr;
        contextOwner.reset();
    }
};
