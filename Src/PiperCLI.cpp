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

#include "Interface/Infrastructure/Allocator.hpp"
#include "Interface/Infrastructure/Concurrency.hpp"
#include "Interface/Infrastructure/Config.hpp"
#include "Interface/Infrastructure/ErrorHandler.hpp"
#include "Interface/Infrastructure/FileSystem.hpp"
#include "Interface/Infrastructure/Logger.hpp"
#include "Interface/Infrastructure/Module.hpp"
#include "Interface/Infrastructure/Operator.hpp"
#include "Interface/Infrastructure/Program.hpp"
#include "PiperContext.hpp"
#include "STL/UniquePtr.hpp"
#include <cstdlib>
#include <cxxopts.hpp>

struct ContextDeleter {
    void operator()(Piper::PiperContextOwner* ptr) const {
        piperDestroyContext(ptr);
    }
};

template <typename T>
static Piper::SharedPtr<T> syncLoad(Piper::PiperContext& context, const Piper::SharedPtr<Piper::Config>& config) {
    auto future = context.getModuleLoader().newInstance(config->at("ClassID")->get<Piper::String>(), config);
    future.wait();
    return eastl::dynamic_shared_pointer_cast<T>(future.get());
}

static void setupInfrastructure(Piper::PiperContextOwner& context, const Piper::SharedPtr<Piper::Config>& config) {
    const auto& attr = config->viewAsObject();

    const auto loggerDesc = attr.find(Piper::String{ "Logger", context.getAllocator() });
    if(loggerDesc != attr.cend()) {
        auto logger = syncLoad<Piper::Logger>(context, loggerDesc->second);
        context.setLogger(std::move(logger));
    }

    const auto allocatorDesc = attr.find(Piper::String{ "Allocator", context.getAllocator() });
    if(allocatorDesc != attr.cend()) {
        auto allocator = syncLoad<Piper::Allocator>(context, allocatorDesc->second);
        context.setAllocator(std::move(allocator));
    }

    const auto schedulerDesc = attr.find(Piper::String{ "Scheduler", context.getAllocator() });
    if(schedulerDesc != attr.cend()) {
        auto scheduler = syncLoad<Piper::Scheduler>(context, schedulerDesc->second);
        context.setScheduler(std::move(scheduler));
    }

    const auto mgrDesc = attr.find(Piper::String{ "PITUManager", context.getAllocator() });
    if(mgrDesc != attr.cend()) {
        auto mgr = syncLoad<Piper::PITUManager>(context, mgrDesc->second);
        context.setPITUManager(std::move(mgr));
    }

    const auto fsDesc = attr.find(Piper::String{ "FileSystem", context.getAllocator() });
    if(fsDesc != attr.cend()) {
        auto fs = syncLoad<Piper::FileSystem>(context, fsDesc->second);
        context.setFileSystem(std::move(fs));
    }
}

static Piper::SharedPtr<Piper::ConfigSerializer> getParser(Piper::PiperContext& context) {
    const auto base = Piper::String{ ".", context.getAllocator() };
    auto name = Piper::makeSharedObject<Piper::Config>(context, "Piper.Infrastructure.NlohmannJson");
    auto path = Piper::makeSharedObject<Piper::Config>(context, "Infrastructure/Config/NlohmannJson/NlohmannJson");
    Piper::UMap<Piper::String, Piper::SharedPtr<Piper::Config>> desc{ context.getAllocator() };
    desc.insert(Piper::makePair(Piper::String{ "Name", context.getAllocator() }, name));
    desc.insert(Piper::makePair(Piper::String{ "Path", context.getAllocator() }, path));
    // TODO:concurrency
    const auto mod = context.getModuleLoader().loadModule(Piper::makeSharedObject<Piper::Config>(context, std::move(desc)), base);
    auto inst = context.getModuleLoader().newInstance("Piper.Infrastructure.NlohmannJson.JsonSerializer", nullptr, mod);
    inst.wait();
    auto parser = eastl::dynamic_shared_pointer_cast<Piper::ConfigSerializer>(inst.get());
    return parser;
}

int main(int argc, char* argv[], [[maybe_unused]] const char* envp[]) {
    Piper::UniquePtr<Piper::PiperContextOwner, ContextDeleter> context(piperCreateContext());
    // TODO:fix resource leak
    try {
        auto opt = cxxopts::Options("PiperCLI", "Piper2 Command Line Interface");
        std::string config, option, command;
        // TODO:custom config parser
        // TODO:user module desc
        opt.add_options()("I,infrastructure", "Infrastructure environment configuration",
                          cxxopts::value(config)->default_value("Infrastructure.json"))(
            "c,command", "Command operator", cxxopts::value(command))("o,option", "command option", cxxopts::value(option));
        opt.parse(argc, argv);

        const auto parser = getParser(*context);

        const auto modules = parser->deserialize(Piper::String{ "Module.json", context->getAllocator() });
        const auto base = Piper::String{ ".", context->getAllocator() };
        for(auto&& desc : modules->viewAsArray())
            context->getModuleLoader().addModuleDescription(desc, base);

        setupInfrastructure(*context, parser->deserialize(Piper::String{ config.c_str(), context->getAllocator() }));

        // TODO:pass environment variable via config

        auto op = context->getModuleLoader().newInstance(Piper::StringView{ command.c_str(), command.size() }, nullptr);
        op.wait();
        auto cop = eastl::dynamic_shared_pointer_cast<Piper::Operator>(op.get());

        cop->execute(parser->deserialize(Piper::String{ option.c_str(), option.size(), context->getAllocator() }));
    } catch(const std::exception& ex) {
        context->getErrorHandler().raiseException(ex.what(), PIPER_SOURCE_LOCATION());
    } catch(...) {
        context->getErrorHandler().raiseException("Unknown Error", PIPER_SOURCE_LOCATION());
    }

    context.reset();
    return EXIT_SUCCESS;
}
