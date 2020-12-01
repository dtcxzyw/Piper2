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

template <typename T, typename S>
static Piper::Future<void> asyncLoad(Piper::PiperContextOwner& context, S call, const Piper::SharedPtr<Piper::Config>& config) {
    auto future = context.getModuleLoader().newInstanceT<T>(config->at("ClassID")->get<Piper::String>(), config);
    return context.getScheduler().spawn([&context, call](Piper::SharedPtr<T> comp) { (context.*call)(std::move(comp)); }, future);
}

static void setupInfrastructure(Piper::PiperContextOwner& context, const Piper::SharedPtr<Piper::Config>& config) {
    const auto& attr = config->viewAsObject();

    Piper::DynamicArray<Piper::Future<void>> futures{ context.getAllocator() };

    const auto loggerDesc = attr.find(Piper::String{ "Logger", context.getAllocator() });
    if(loggerDesc != attr.cend())
        futures.push_back(asyncLoad<Piper::Logger>(context, &Piper::PiperContextOwner::setLogger, loggerDesc->second));

    const auto allocatorDesc = attr.find(Piper::String{ "Allocator", context.getAllocator() });
    if(allocatorDesc != attr.cend())
        futures.push_back(asyncLoad<Piper::Allocator>(context, &Piper::PiperContextOwner::setAllocator, allocatorDesc->second));

    const auto schedulerDesc = attr.find(Piper::String{ "Scheduler", context.getAllocator() });
    if(schedulerDesc != attr.cend())
        futures.push_back(asyncLoad<Piper::Scheduler>(context, &Piper::PiperContextOwner::setScheduler, schedulerDesc->second));

    const auto mgrDesc = attr.find(Piper::String{ "PITUManager", context.getAllocator() });
    if(mgrDesc != attr.cend())
        futures.push_back(asyncLoad<Piper::PITUManager>(context, &Piper::PiperContextOwner::setPITUManager, mgrDesc->second));

    const auto fsDesc = attr.find(Piper::String{ "FileSystem", context.getAllocator() });
    if(fsDesc != attr.cend())
        futures.push_back(asyncLoad<Piper::FileSystem>(context, &Piper::PiperContextOwner::setFileSystem, fsDesc->second));

    for(auto&& future : futures)
        future.wait();
}

static Piper::SharedPtr<Piper::ConfigSerializer> getParser(Piper::PiperContext& context, Piper::StringView modulePath,
                                                           Piper::StringView parserName) {
    const auto base = Piper::String{ ".", context.getAllocator() };

    const auto pos = parserName.find_last_of('.');
    if(pos == Piper::StringView::npos)
        context.getErrorHandler().raiseException(
            "Invalid classID \"" + Piper::String{ parserName, context.getAllocator() } + "\".", PIPER_SOURCE_LOCATION());

    auto name = Piper::makeSharedObject<Piper::Config>(context, parserName.substr(0, pos));
    auto path = Piper::makeSharedObject<Piper::Config>(context, modulePath);
    Piper::UMap<Piper::String, Piper::SharedPtr<Piper::Config>> desc{ context.getAllocator() };
    desc.insert(Piper::makePair(Piper::String{ "Name", context.getAllocator() }, name));
    desc.insert(Piper::makePair(Piper::String{ "Path", context.getAllocator() }, path));

    const auto mod = context.getModuleLoader().loadModule(Piper::makeSharedObject<Piper::Config>(context, std::move(desc)), base);
    auto parser = context.getModuleLoader().newInstanceT<Piper::ConfigSerializer>(parserName, nullptr, mod);
    parser.wait();
    return parser.get();
}

// environment variable is not supported
int main(int argc, char* argv[]) {
    Piper::UniquePtr<Piper::PiperContextOwner, ContextDeleter> context(piperCreateContext());
    // TODO:fix resource leak
    try {
        auto opt = cxxopts::Options("PiperCLI", "Piper2 Command Line Interface");
        std::string config, option, command, modulePath, parserName, moduleDesc;
        opt.add_options()("I,infrastructure", "Infrastructure environment configuration",
                          cxxopts::value(config)->default_value("Infrastructure.json"))(
            "c,command", "Command operator", cxxopts::value(command))("o,option", "command option", cxxopts::value(option))(
            "m,module", "Parser module path",
            cxxopts::value(modulePath)->default_value("Infrastructure/Config/NlohmannJson/NlohmannJson"))(
            "n,name", "Parser name",
            cxxopts::value(parserName)->default_value("Piper.Infrastructure.NlohmannJson.JsonSerializer"))(
            "d,desc", "Basic module desc file", cxxopts::value(moduleDesc)->default_value("Module.json"));
        opt.parse(argc, argv);
        // TODO:help switch

        const auto parser = getParser(*context, Piper::StringView{ modulePath.data(), modulePath.size() },
                                      Piper::StringView{ parserName.data(), parserName.size() });

        const auto modules = parser->deserialize(
            Piper::String{ Piper::StringView{ moduleDesc.data(), moduleDesc.size() }, context->getAllocator() });
        const auto base = Piper::String{ ".", context->getAllocator() };
        for(auto&& desc : modules->viewAsArray())
            context->getModuleLoader().addModuleDescription(desc, base);

        setupInfrastructure(*context, parser->deserialize(Piper::String{ config.c_str(), context->getAllocator() }));

        auto op = context->getModuleLoader().newInstanceT<Piper::Operator>(Piper::StringView{ command.c_str(), command.size() },
                                                                           nullptr);

        auto future = PIPER_FUTURE_CALL(op, execute)(
            parser->deserialize(Piper::String{ option.c_str(), option.size(), context->getAllocator() }));
        future.wait();
    } catch(const std::exception& ex) {
        context->getErrorHandler().raiseException(ex.what(), PIPER_SOURCE_LOCATION());
    } catch(...) {
        context->getErrorHandler().raiseException("Unknown Error", PIPER_SOURCE_LOCATION());
    }

    context.reset();
    return EXIT_SUCCESS;
}
