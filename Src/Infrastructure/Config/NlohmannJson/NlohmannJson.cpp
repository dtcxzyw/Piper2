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
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include <new>
#include <nlohmann/json.hpp>

// TODO:use STLAllocator/String
using Json = nlohmann::json;

namespace Piper {
    class JsonSerializer final : public ConfigSerializer {
    private:
        SharedPtr<Config> buildFromJson(const Json& json) const {
            switch(json.type()) {
                case Json::value_t::null:
                    return makeSharedObject<Config>(context(), MonoState{});
                case Json::value_t::object: {
                    UMap<String, SharedPtr<Config>> attrs{ context().getAllocator() };
                    for(const auto& attr : json.items()) {
                        auto&& key = attr.key();
                        attrs.insert(makePair(String{ StringView{ key.c_str(), key.size() }, context().getAllocator() },
                                              buildFromJson(attr.value())));
                    }
                    return makeSharedObject<Config>(context(), std::move(attrs));
                }
                case Json::value_t::array: {
                    Vector<SharedPtr<Config>> elements{ context().getAllocator() };
                    elements.reserve(json.size());
                    for(const auto& element : json)
                        elements.push_back(buildFromJson(element));
                    return makeSharedObject<Config>(context(), std::move(elements));
                }
                case Json::value_t::string: {
                    auto str = json.get<std::string>();
                    return makeSharedObject<Config>(context(), StringView{ str.c_str(), str.size() });
                }
                case Json::value_t::boolean:
                    return makeSharedObject<Config>(context(), json.get<bool>());
                case Json::value_t::number_integer:
                    return makeSharedObject<Config>(context(), json.get<intmax_t>());
                case Json::value_t::number_unsigned:
                    return makeSharedObject<Config>(context(), json.get<uintmax_t>());
                case Json::value_t::number_float:
                    return makeSharedObject<Config>(context(), json.get<double>());
                case Json::value_t::binary:
                    context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
                case Json::value_t::discarded:
                    context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            }
            // TODO:disable warining C4715
        }

        Json buildJson(const SharedPtr<Config>& config) const {
            switch(config->type()) {
                case NodeType::FloatingPoint:
                    return config->get<double>();
                case NodeType::String:
                    return config->get<String>().c_str();
                case NodeType::SignedInteger:
                    return config->get<intmax_t>();
                case NodeType::UnsignedInteger:
                    return config->get<uintmax_t>();
                case NodeType::Boolean:
                    return config->get<bool>();
                case NodeType::Array: {
                    Json res;
                    for(auto&& element : config->viewAsArray())
                        res.push_back(buildJson(element));
                    return std::move(res);
                }
                case NodeType::Object: {
                    Json res;
                    for(auto&& attr : config->viewAsObject())
                        res[attr.first.c_str()] = buildJson(attr.second);
                    return std::move(res);
                }
                case NodeType::Null:
                    return {};
            }
            // TODO:disable warning C4715
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(JsonSerializer, ConfigSerializer)
        SharedPtr<Config> deserialize(const String& path) const override {
            auto stage = context().getErrorHandler().enterStage("parse configuration " + path, PIPER_SOURCE_LOCATION());
            auto file = context().getFileSystem().mapFile(path, FileAccessMode::Read, FileCacheHint::Sequential);
            auto map = file->map(0, file->size());
            auto span = map->get();
            auto beg = reinterpret_cast<char8_t*>(span.data()), end = beg + span.size();
            // TODO:parser callback
            auto json = Json::parse(beg, end);
            stage.switchToStatic("build from json", PIPER_SOURCE_LOCATION());
            return buildFromJson(json);
        }
        void serialize(const SharedPtr<Config>& config, const String& path) const override {
            auto stage = context().getErrorHandler().enterStageStatic("build json from configuration", PIPER_SOURCE_LOCATION());
            auto content = buildJson(config).dump();
            stage.switchTo("output json to " + path, PIPER_SOURCE_LOCATION());
            auto file = context().getFileSystem().mapFile(path, FileAccessMode::Write, FileCacheHint::Sequential, content.size());
            auto map = file->map(0, file->size());
            auto span = map->get();
            memcpy(span.data(), content.c_str(), content.size());
        }
    };
    class ModuleImpl final : public Module {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                                 const Future<void>& module) override {
            if(classID == "JsonSerializer") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<JsonSerializer>(context())));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
