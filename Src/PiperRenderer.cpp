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

#define PIPER_EXPORT
#include "Interface/BuiltinComponent/Geometry.hpp"
#include "Interface/BuiltinComponent/Integrator.hpp"
#include "Interface/BuiltinComponent/Light.hpp"
#include "Interface/BuiltinComponent/RenderDriver.hpp"
#include "Interface/BuiltinComponent/Sampler.hpp"
#include "Interface/BuiltinComponent/Sensor.hpp"
#include "Interface/BuiltinComponent/StructureParser.hpp"
#include "Interface/BuiltinComponent/Surface.hpp"
#include "Interface/BuiltinComponent/Tracer.hpp"
#include "Interface/Infrastructure/ErrorHandler.hpp"
#include "Interface/Infrastructure/Module.hpp"
#include "Interface/Infrastructure/Operator.hpp"
#include "Kernel/Protocol.hpp"
#pragma warning(push, 0)
#define _CRT_SECURE_NO_WARNINGS
#include <OpenEXR/ImfRgbaFile.h>
#undef _CRT_SECURE_NO_WARNINGS
#pragma warning(pop)

#include <filesystem>

namespace Piper {
    // TODO: instancing of lights and geometries
    // TODO: JPG/PNG/DNG/WebP/HEIF/EXIF/Apple ProRes RAW/CinemaDNG/Cineform RAW output
    // EXR: HDR
    // JPG/PNG/WebP/HEIF/PPM: exchange on the Internet (lossy/lossless)
    // DNG/EXIF/Apple ProRes RAW/CinemaDNG/Cineform RAW: HDR RAW lossless
    // Spectrum Array

    class Render final : public Operator {
    private:
        [[nodiscard]] FitMode str2FitMode(const String& mode) const {
            if(mode == "Fill")
                return FitMode::Fill;
            if(mode == "OverScan")
                return FitMode::OverScan;
            context().getErrorHandler().raiseException("Invalid FitMode \"" + mode + "\".", PIPER_SOURCE_LOCATION());
        }

        [[nodiscard]] TransformSRT parseTransform(const SharedPtr<Config>& config) const {
            TransformSRT transform{};

            auto reportError = [this] {
                context().getErrorHandler().raiseException("Invalid transform", PIPER_SOURCE_LOCATION());
            };
            // SRT
            const auto& attrs = config->viewAsObject();
            const auto translation = attrs.find(String{ "Translation", context().getAllocator() });
            if(translation != attrs.cend()) {
                const auto offset = parseVector<float, FOR::World>(translation->second);
                transform.transX = offset.x;
                transform.transY = offset.y;
                transform.transZ = offset.z;
            } else
                transform.transX = transform.transY = transform.transZ = 0.0f;

            const auto scale = attrs.find(String{ "Scale", context().getAllocator() });
            transform.skewXY = transform.skewXZ = transform.skewYZ = 0.0f;
            transform.shiftX = transform.shiftY = transform.shiftZ = 0.0f;
            if(scale != attrs.cend()) {
                const auto& desc = scale->second;
                switch(desc->type()) {
                    case NodeType::FloatingPoint: {
                        transform.scaleX = transform.scaleY = transform.scaleZ = static_cast<float>(desc->get<double>());
                    } break;
                    case NodeType::Array: {
                        const auto factor = parseVector<float, FOR::World>(desc);
                        transform.scaleX = factor.x;
                        transform.scaleY = factor.y;
                        transform.scaleZ = factor.z;
                    } break;
                    case NodeType::Object: {
                        const auto factor = parseVector<float, FOR::World>(desc->at("Scale"));
                        transform.scaleX = factor.x;
                        transform.scaleY = factor.y;
                        transform.scaleZ = factor.z;
                        const auto skew = parseVector<float, FOR::World>(desc->at("Skew"));
                        transform.skewXY = skew.x;
                        transform.skewXZ = skew.y;
                        transform.skewYZ = skew.z;
                        const auto shift = parseVector<float, FOR::World>(desc->at("Shift"));
                        transform.shiftX = shift.x;
                        transform.shiftY = shift.y;
                        transform.shiftZ = shift.z;
                    } break;
                    default:
                        reportError();
                }
            } else {
                transform.scaleX = transform.scaleY = transform.scaleZ = 1.0f;
            }
            const auto rotation = attrs.find(String{ "Rotation", context().getAllocator() });
            if(rotation != attrs.cend()) {
                const auto& arr = rotation->second->viewAsArray();
                if(arr.size() == 3) {
                    // euler angle
                    const auto halfPitch = 0.5f * static_cast<float>(arr[0]->get<double>());
                    const auto chPitch = std::cos(halfPitch), shPitch = std::sin(halfPitch);

                    const auto halfYaw = 0.5f * static_cast<float>(arr[1]->get<double>());
                    const auto chYaw = std::cos(halfYaw), shYaw = std::sin(halfYaw);

                    const auto halfRoll = 0.5f * static_cast<float>(arr[2]->get<double>());
                    const auto chRoll = std::cos(halfRoll), shRoll = std::sin(halfRoll);

                    transform.quatW = chRoll * chPitch * chYaw + shRoll * shPitch * shYaw;
                    transform.quatX = chRoll * shPitch * chYaw + shRoll * chPitch * shYaw;
                    transform.quatY = chRoll * chPitch * shYaw - shRoll * shPitch * chYaw;
                    transform.quatZ = shRoll * chPitch * chYaw - chRoll * shPitch * shYaw;
                } else if(arr.size() == 4) {
                    transform.quatW = static_cast<float>(arr[0]->get<double>());
                    transform.quatX = static_cast<float>(arr[1]->get<double>());
                    transform.quatY = static_cast<float>(arr[2]->get<double>());
                    transform.quatZ = static_cast<float>(arr[3]->get<double>());
                } else
                    reportError();
            } else {
                transform.quatW = 1.0f;
                transform.quatX = transform.quatY = transform.quatZ = 0.0f;
            }
            return transform;
        }

        // TODO:concurrency
        template <typename T>
        [[nodiscard]] SharedPtr<T> syncLoad(const SharedPtr<Config>& config) const {
            return context().getModuleLoader().newInstanceT<T>(config).getSync();
        }

        // TODO:redirect of nodes
        // TODO:redirect of scenes
        SharedPtr<Node> buildSceneImpl(Tracer& tracer, const SharedPtr<Config>& config,
                                       UMap<String, SharedPtr<Node>>& namedNodes) const {
            const auto type = config->at("Type")->get<String>();
            if(type == "Instance") {
                const auto& attr = config->viewAsObject();
                if(attr.count(String{ "ClassID" }))
                    return tracer.buildNode(syncLoad<Object>(config));

                auto geometry = syncLoad<Geometry>(config->at("Geometry"));
                auto surface = syncLoad<Surface>(config->at("Surface"));
                // TODO:medium
                return tracer.buildNode(tracer.buildGSMInstance(std::move(geometry), std::move(surface), nullptr));
            }

            if(type == "Group") {
                const auto& sub = config->at("Children")->viewAsArray();
                DynamicArray<Pair<TransformInfo, SharedPtr<Node>>> nodes{ context().getAllocator() };
                for(auto&& inst : sub) {
                    TransformInfo transform{ Time<float>{ 0.0f }, Time<float>{ 0.0f },
                                             DynamicArray<Pair<uint32_t, TransformSRT>>{ context().getAllocator() } };
                    const auto& attr = inst->viewAsObject();
                    const auto iter = attr.find(String{ "Transform", context().getAllocator() });
                    if(iter != attr.cend()) {
                        const auto& transformAttr = iter->second->viewAsObject();
                        const auto offset = transformAttr.find(String{ "Offset", context().getAllocator() });
                        if(offset != transformAttr.cend()) {
                            // dynamic transform
                            transform.offset = { static_cast<float>(offset->second->get<double>()) };
                            transform.step = { static_cast<float>(iter->second->at("Step")->get<double>()) };
                            for(auto&& trans : iter->second->at("KeyFrames")->viewAsArray()) {
                                transform.transforms.emplace_back(
                                    makePair(static_cast<uint32_t>(trans->at("Key")->get<uintmax_t>()),
                                             parseTransform(trans->at("Transform"))));
                            }

                            if(transform.transforms.size() < 2)
                                context().getErrorHandler().raiseException("Invalid dynamic transform", PIPER_SOURCE_LOCATION());
                            // TODO: use Validation Layer
                            for(uint32_t idx = 1; idx < transform.transforms.size(); ++idx)
                                if(transform.transforms[idx].first <= transform.transforms[idx - 1].first) {
                                    context().getErrorHandler().raiseException("Invalid dynamic transform",
                                                                               PIPER_SOURCE_LOCATION());
                                }
                        } else {
                            // static transform
                            transform.transforms.emplace_back(makePair(0U, parseTransform(iter->second)));
                        }
                    }
                    nodes.emplace_back(makePair(std::move(transform), buildScene(tracer, inst, namedNodes)));
                }
                return tracer.buildNode(nodes);
            }

            context().getErrorHandler().raiseException("Invalid scene node.", PIPER_SOURCE_LOCATION());
        }

        [[nodiscard]] SharedPtr<Node> buildScene(Tracer& tracer, const SharedPtr<Config>& config,
                                                 UMap<String, SharedPtr<Node>>& namedNodes) const {
            const auto& attr = config->viewAsObject();
            const auto name = attr.find(String{ "ID", context().getAllocator() });
            auto node = buildSceneImpl(tracer, config, namedNodes);
            if(name != attr.cend())
                namedNodes.emplace(makePair(name->second->get<String>(), node));
            return node;
        }

        template <typename T, typename U>
        SharedPtr<T> requireObject(const UMap<String, SharedPtr<U>>& objects, const String& key) const {
            const auto iter = objects.find(key);
            if(iter == objects.cend())
                context().getErrorHandler().raiseException("Failed to find object named \"" + key + "\".",
                                                           PIPER_SOURCE_LOCATION());
            if constexpr(std::is_same_v<T, U>)
                return iter->second;
            else
                return eastl::dynamic_shared_pointer_cast<T>(iter->second);
        }

        // TODO:dynamic parameters of sensor
        struct ActionInfo final {
            String activeSensor;
            // String follow;
            Time<float> timeBegin;
            Time<float> shutterOpenTime;
            Time<float> shutterCloseTime;
            uint32_t frameCount;
            FitMode fitMode;
        };

        [[nodiscard]] DynamicArray<ActionInfo> parseActions(const SharedPtr<Config>& action) const {
            DynamicArray<ActionInfo> res{ context().getAllocator() };
            for(auto&& desc : action->viewAsArray()) {
                res.push_back({ desc->at("ActiveSensor")->get<String>(),
                                { static_cast<float>(desc->at("TimeBegin")->get<double>()) },
                                { static_cast<float>(desc->at("ShutterOpenTime")->get<double>()) },
                                { static_cast<float>(desc->at("ShutterCloseTime")->get<double>()) },
                                static_cast<uint32_t>(desc->at("FrameCount")->get<uintmax_t>()),
                                str2FitMode(desc->at("FitMode")->get<String>()) });
            }
            return res;
        }

        UniqueObject<Pipeline> buildPipeline(Tracer& tracer, RenderDriver& renderDriver, const SharedPtr<Config>& config,
                                             UMap<String, SharedPtr<Node>>& namedNodes) const {
            auto stage = context().getErrorHandler().enterStage("initialize objects", PIPER_SOURCE_LOCATION());
            // TODO:redirect context
            // TODO:concurrency
            UMap<String, SharedPtr<Object>> objects{ context().getAllocator() };
            for(auto&& obj : config->at("Objects")->viewAsArray()) {
                objects.emplace(obj->at("ID")->get<String>(), context().getModuleLoader().newInstance(obj).getSync());
            }
            stage.next("build scene hierarchy", PIPER_SOURCE_LOCATION());
            const auto scene = buildScene(tracer, config->at("Scene"), namedNodes);

            stage.next("initialize global components", PIPER_SOURCE_LOCATION());
            const auto lightSampler = syncLoad<LightSampler>(config->at("LightSampler"));
            const auto integrator = syncLoad<Integrator>(config->at("Integrator"));
            const auto sampler = syncLoad<Sampler>(config->at("Sampler"));

            return tracer.buildPipeline(scene, *integrator, renderDriver, *lightSampler, sampler);
        }
        void saveImage(const String& dest, const DynamicArray<Spectrum<Radiance>>& res, const uint32_t width,
                       const uint32_t height) const {
            // TODO:move OpenEXR to ImageIO
            DynamicArray<Imf::Rgba> rgba(res.size(), context().getAllocator());
            auto invalid = false;
            eastl::transform(res.cbegin(), res.cend(), rgba.begin(), [&](const Spectrum<Radiance> rad) {
                if(isfinite(rad.r.val) && isfinite(rad.g.val) && isfinite(rad.b.val) &&
                   fminf(rad.r.val, fminf(rad.g.val, rad.b.val)) >= 0.0f)
                    return Imf::Rgba{ rad.r.val, rad.g.val, rad.b.val };
                invalid = true;
                return Imf::Rgba{ 1.0f, 0.0f, 0.0f };
            });
            if(invalid && context().getLogger().allow(LogLevel::Warning)) {
                context().getLogger().record(LogLevel::Warning, "Some invalid pixels are detected.", PIPER_SOURCE_LOCATION());
            }
            // TODO:filesystem

            Imf::RgbaOutputFile out((dest + ".exr").c_str(), width, height, Imf::WRITE_RGB);
            out.setFrameBuffer(rgba.data(), 1, width);
            out.writePixels(height);
        }

        void saveToFile(const String& dest, const DynamicArray<DynamicArray<Spectrum<Radiance>>>& frames, const uint32_t width,
                        const uint32_t height) const {
            // TODO:filesystem
            namespace fs = std::filesystem;
            if(!fs::exists(dest.c_str()) && !fs::create_directories(dest.c_str()))
                context().getErrorHandler().raiseException("Failed to create directory \"" + dest + "\"",
                                                           PIPER_SOURCE_LOCATION());
            for(uint32_t idx = 0; idx < frames.size(); ++idx)
                saveImage(dest + "/Frame" + toString(context().getAllocator(), idx), frames[idx], width, height);
        }

    public:
        explicit Render(PiperContext& context) : Operator(context) {}
        // TODO:scene module desc
        void execute(const SharedPtr<Config>& opt) override {
            auto stage = context().getErrorHandler().enterStage("parse option", PIPER_SOURCE_LOCATION());
            // TODO:set workspace
            auto sizeDesc = opt->at("ImageSize")->viewAsArray();
            if(sizeDesc.size() != 2)
                context().getErrorHandler().raiseException("Invalid ImageSize.", PIPER_SOURCE_LOCATION());
            // TODO:directly get DynamicArray from Config
            const auto width = static_cast<uint32_t>(sizeDesc[0]->get<uintmax_t>());
            const auto height = static_cast<uint32_t>(sizeDesc[1]->get<uintmax_t>());
            const auto scenePath = opt->at("SceneDesc")->get<String>();
            auto parserID = opt->at("SceneParser")->get<String>();

            auto parser = context().getModuleLoader().newInstanceT<ConfigSerializer>(parserID);

            auto scene = parser.getSync()->deserialize(scenePath);
            stage.next("initialize pipeline", PIPER_SOURCE_LOCATION());
            const auto tracer = syncLoad<Tracer>(opt->at("Tracer"));
            auto renderDriver = syncLoad<RenderDriver>(scene->at("RenderDriver"));

            const auto actions = parseActions(scene->at("Actions"));

            UMap<String, SharedPtr<Node>> namedNodes{ context().getAllocator() };
            const auto pipeline = buildPipeline(*tracer, *renderDriver, scene, namedNodes);

            stage.next("start rendering", PIPER_SOURCE_LOCATION());

            const auto output = opt->at("OutputDir")->get<String>();
            uint32_t idx = 0;
            for(auto&& action : actions) {
                auto frameStage = context().getErrorHandler().enterStage("prepare for rendering", PIPER_SOURCE_LOCATION());
                auto sensor = namedNodes.find(action.activeSensor);
                if(sensor == namedNodes.cend())
                    context().getErrorHandler().raiseException(
                        "Failed to find a node with sensor named \"" + action.activeSensor + "\".", PIPER_SOURCE_LOCATION());

                const auto launcher = pipeline->prepare(sensor->second, width, height, action.fitMode);

                // TODO: streaming rendering+image output/video encoding
                DynamicArray<DynamicArray<Spectrum<Radiance>>> frames{ context().getAllocator() };
                for(uint32_t i = 0; i < action.frameCount; ++i) {
                    // TODO: better parameters
                    const auto begin = action.timeBegin +
                        Dimensionless<float>{ static_cast<float>(i) } * (action.shutterOpenTime + action.shutterCloseTime);
                    const auto end = begin + action.shutterOpenTime;
                    launcher->updateTimeInterval(begin, end);
                    // TODO: better interface
                    auto res = renderDriver->renderFrame(*tracer, *launcher);
                    frames.push_back(std::move(res));
                }

                // TODO: write video
                saveToFile(output + "/Action" + toString(context().getAllocator(), idx++), frames, width, height);
            }
            if(context().getLogger().allow(LogLevel::Info))
                context().getLogger().record(LogLevel::Info, pipeline->generateStatisticsReport(), PIPER_SOURCE_LOCATION());
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        explicit ModuleImpl(PiperContext& context, const char* path) : Module(context), mPath(path, context.getAllocator()) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Render") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Render>(context())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
