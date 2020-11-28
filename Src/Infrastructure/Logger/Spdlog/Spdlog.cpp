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

#define PIPER_EXPORT
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/Logger.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <map>
#include <new>
#include <spdlog/spdlog.h>

namespace Piper {
    class Spdlog final : public Logger {
    private:
        LogLevel mLevel;

    public:
        explicit Spdlog(PiperContext& context, const SharedPtr<Config>& config)
            : Logger(context), mLevel(static_cast<LogLevel>(0)) {
            static const std::map<size_t, LogLevel> map{ { hashStringView("Debug"), LogLevel::Debug },
                                                         { hashStringView("Error"), LogLevel::Error },
                                                         { hashStringView("Fatal"), LogLevel::Fatal },
                                                         { hashStringView("Info"), LogLevel::Info },
                                                         { hashStringView("Warning"), LogLevel::Warning } };
            auto&& settings = config->viewAsObject();
            const auto iter = settings.find(String("level", context.getAllocator()));
            if(iter != settings.cend()) {
                for(auto&& level : iter->second->viewAsArray()) {
                    auto key = hashStringView(level->get<String>());
                    auto val = map.find(key);
                    if(val != map.cend())
                        mLevel = static_cast<LogLevel>(static_cast<uint32_t>(mLevel) | static_cast<uint32_t>(val->second));
                    else
                        throw;
                }
            } else
                mLevel = static_cast<LogLevel>(31);
            // TODO:format
            // TODO:color
        }
        [[nodiscard]] bool allow(const LogLevel level) const noexcept override {
            return static_cast<uint32_t>(mLevel) & static_cast<uint32_t>(level);
        }
        [[nodiscard]] void record(const LogLevel level, const StringView& message,
                                  const SourceLocation& sourceLocation) noexcept override {
            if(!allow(level))
                return;
            auto castLevel = [](const LogLevel lv) -> spdlog::level::level_enum {
                switch(lv) {
                    case LogLevel::Info:
                        return spdlog::level::info;
                    case LogLevel::Warning:
                        return spdlog::level::warn;
                    case LogLevel::Error:
                        return spdlog::level::err;
                    case LogLevel::Fatal:
                        return spdlog::level::critical;
                    case LogLevel::Debug:
                        return spdlog::level::debug;
                }
                throw;
            };
            const auto loc = spdlog::source_loc{ sourceLocation.file, sourceLocation.line, sourceLocation.func };
            spdlog::log(loc, castLevel(level), std::string_view{ message.data(), message.size() });
        }
        void flush() noexcept override {
            spdlog::default_logger()->flush();
        }
    };
    class ModuleImpl final : public Module {
    public:
        ModuleImpl(PiperContext& context, const char*) : Module(context) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Spdlog") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Spdlog>(context(), config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
