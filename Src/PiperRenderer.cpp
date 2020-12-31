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

namespace Piper {
    enum class FitMode { Fill, OverScan };
    FitMode str2FitMode(PiperContext& context, const String& mode) {
        if(mode == "Fill")
            return FitMode::Fill;
        if(mode == "OverScan")
            return FitMode::OverScan;
        context.getErrorHandler().raiseException("Invalid FitMode \"" + mode + "\".", PIPER_SOURCE_LOCATION());
    }

    class Render final : public Operator {
    private:
        static auto parseTransform(const SharedPtr<Config>& config) {
            Transform<Distance, FOR::Local, FOR::World> transform{};
            auto& context = config->context();
            auto reportError = [&context] {
                context.getErrorHandler().raiseException("Invalid transform", PIPER_SOURCE_LOCATION());
            };
            if(config->type() == NodeType::Array) {
                const auto& arr = config->viewAsArray();
                if(arr.size() != 3)
                    reportError();
                for(uint32_t i = 0; i < 3; ++i) {
                    if(arr[i]->type() != NodeType::Array)
                        reportError();
                    const auto& row = arr[i]->viewAsArray();
                    if(row.size() != 4)
                        reportError();
                    for(uint32_t j = 0; j < 4; ++j) {
                        transform.A2B[i][j].val = static_cast<float>(row[j]->get<double>());
                    }
                }
            } else if(config->type() == NodeType::Object) {
                // SRT
                const auto& attrs = config->viewAsObject();
                const auto translation = attrs.find(String{ "Translation", context.getAllocator() });
                if(translation != attrs.cend()) {
                    const auto offset = parseVector<Dimensionless<float>, FOR::World>(translation->second);
                    transform.A2B[0][3] = offset.x;
                    transform.A2B[1][3] = offset.y;
                    transform.A2B[2][3] = offset.z;
                }
                const auto scale = attrs.find(String{ "Scale", context.getAllocator() });
                if(scale != attrs.cend()) {
                    const auto mode = scale->second->type();
                    if(mode == NodeType::FloatingPoint) {
                        transform.A2B[0][0] = transform.A2B[1][1] = transform.A2B[2][2] =
                            Dimensionless<float>{ static_cast<float>(scale->second->get<double>()) };
                    } else if(mode == NodeType::Array) {
                        const auto factor = parseVector<Dimensionless<float>, FOR::World>(scale->second);
                        transform.A2B[0][0] = factor.x;
                        transform.A2B[1][1] = factor.y;
                        transform.A2B[2][2] = factor.z;
                    } else
                        reportError();
                } else {
                    transform.A2B[0][0] = transform.A2B[1][1] = transform.A2B[2][2] = Dimensionless<float>{ 1.0f };
                }
                const auto rotation = attrs.find(String{ "Rotation", context.getAllocator() });
                if(rotation != attrs.cend()) {
                    context.getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
                }
            } else
                reportError();
            calcInverse(transform.A2B, transform.B2A);
            return transform;
        }

        // TODO:concurrency
        template <typename T>
        SharedPtr<T> syncLoad(const SharedPtr<Config>& config) {
            auto future = context().getModuleLoader().newInstance(config->at("ClassID")->get<String>(), config);
            future.wait();
            return eastl::dynamic_shared_pointer_cast<T>(future.get());
        }

        SharedPtr<Node> buildScene(Tracer& tracer, const SharedPtr<Config>& config) {

            const auto type = config->at("Type")->get<String>();
            if(type == "Instance") {
                GSMInstanceDesc desc;
                desc.surface = syncLoad<Surface>(config->at("Surface"));
                desc.geometry = syncLoad<Geometry>(config->at("Geometry"));
                const auto& attr = config->viewAsObject();
                const auto iter = attr.find(String{ "Transform", context().getAllocator() });
                if(iter != attr.cend())
                    desc.transform = parseTransform(iter->second);
                return tracer.buildNode(std::move(desc));
            }
            if(type == "Group") {
                const auto& sub = config->at("Children")->viewAsArray();
                GroupDesc desc;
                desc.nodes.set_allocator(context().getAllocator());
                for(auto&& inst : sub)
                    desc.nodes.emplace_back(buildScene(tracer, inst));

                auto&& attr = config->viewAsObject();
                const auto iter = attr.find(String{ "Transform", context().getAllocator() });
                if(iter != attr.cend())
                    desc.transform = parseTransform(iter->second);
                return tracer.buildNode(std::move(desc));
            }

            context().getErrorHandler().raiseException("Invalid scene node.", PIPER_SOURCE_LOCATION());
        }

        UniqueObject<Pipeline> buildPipeline(Tracer& tracer, RenderDriver& renderDriver, const SharedPtr<Config>& config,
                                             const uint32_t width, const uint32_t height, float& ratio) {
            // TODO:asset
            const auto sensor = syncLoad<Sensor>(config->at("Sensor"));
            ratio = sensor->getAspectRatio();
            const auto lightSampler = syncLoad<LightSampler>(config->at("LightSampler"));
            DynamicArray<SharedPtr<Light>> lights(context().getAllocator());
            lights.push_back(syncLoad<Light>(config->at("Environment")));
            for(auto&& light : config->at("Lights")->viewAsArray())
                lights.push_back(syncLoad<Light>(light));
            lightSampler->preprocess({ lights.cbegin(), lights.cend() });
            const auto integrator = syncLoad<Integrator>(config->at("Integrator"));
            const auto node = buildScene(tracer, config->at("Scene"));
            const auto& attr = config->viewAsObject();
            const auto samplerDesc = attr.find(String{ "Sampler", context().getAllocator() });
            const auto sampler = samplerDesc != attr.cend() ? syncLoad<Sampler>(samplerDesc->second) : SharedPtr<Sampler>{};
            return tracer.buildPipeline(node, *sensor, *integrator, renderDriver, *lightSampler, lights, sampler.get(), width,
                                        height);
        }
        void saveToFile(const String& dest, const DynamicArray<Spectrum<Radiance>>& res, const uint32_t width,
                        const uint32_t height) const {
            DynamicArray<Imf::Rgba> rgba(res.size(), context().getAllocator());

            eastl::transform(res.cbegin(), res.cend(), rgba.begin(), [](const Spectrum<Radiance> rad) {
                if(isfinite(rad.r.val) && isfinite(rad.g.val) && isfinite(rad.b.val) &&
                   fminf(rad.r.val, fminf(rad.g.val, rad.b.val)) >= 0.0f)
                    return Imf::Rgba{ rad.r.val, rad.g.val, rad.b.val };
                return Imf::Rgba{ 1.0f, 0.0f, 0.0f };
            });
            // TODO:filesystem
            Imf::RgbaOutputFile out(dest.c_str(), width, height, Imf::WRITE_RGB);
            out.setFrameBuffer(rgba.data(), 1, width);
            out.writePixels(height);
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
            auto width = static_cast<uint32_t>(sizeDesc[0]->get<uintmax_t>());
            auto height = static_cast<uint32_t>(sizeDesc[1]->get<uintmax_t>());
            auto scenePath = opt->at("SceneDesc")->get<String>();
            auto parserID = opt->at("SceneParser")->get<String>();
            auto fitMode = str2FitMode(context(), opt->at("FitMode")->get<String>());

            auto parser = context().getModuleLoader().newInstanceT<ConfigSerializer>(parserID, nullptr);
            parser.wait();

            auto scene = parser.get()->deserialize(scenePath);
            stage.next("initialize pipeline", PIPER_SOURCE_LOCATION());
            auto tracer = syncLoad<Tracer>(opt->at("Tracer"));
            auto renderDriver = syncLoad<RenderDriver>(scene->at("RenderDriver"));

            auto deviceAspectRatio = -1.0f;
            auto pipeline = buildPipeline(*tracer, *renderDriver, scene, width, height, deviceAspectRatio);

            RenderRECT rect{};
            SensorNDCAffineTransform transform{};
            auto imageAspectRatio = static_cast<float>(width) / static_cast<float>(height);
            auto iiar = 1.0f / imageAspectRatio, idar = 1.0f / deviceAspectRatio;
            if(fitMode == FitMode::Fill) {
                rect = { 0, 0, width, height };
                if(imageAspectRatio > deviceAspectRatio) {
                    transform = { 0.0f, (idar - iiar) * 0.5f * deviceAspectRatio, 1.0f, iiar * deviceAspectRatio };
                } else {
                    transform = { (deviceAspectRatio - imageAspectRatio) * 0.5f * idar, 0.0f, imageAspectRatio * idar, 1.0f };
                }
            } else {
                if(imageAspectRatio > deviceAspectRatio) {
                    transform = { -(imageAspectRatio - deviceAspectRatio) * 0.5f * idar, 0.0f, imageAspectRatio * idar, 1.0f };
                    rect = { static_cast<uint32_t>(floorf(std::max(
                                 0.0f, static_cast<float>(width) * (imageAspectRatio - deviceAspectRatio) * 0.5f * iiar))),
                             0,
                             std::min(width,
                                      static_cast<uint32_t>(ceilf(static_cast<float>(width) *
                                                                  (imageAspectRatio + deviceAspectRatio) * 0.5f * iiar))),
                             height };
                    rect.width -= rect.left;
                } else {
                    transform = { 0.0f, -(iiar - idar) * 0.5f * deviceAspectRatio, 1.0f, deviceAspectRatio * iiar };
                    rect = { 0,
                             static_cast<uint32_t>(
                                 floorf(std::max(0.0f, static_cast<float>(height) * (iiar - idar) * 0.5f * imageAspectRatio))),
                             width,
                             static_cast<uint32_t>(ceilf(static_cast<float>(height) * (iiar + idar) * 0.5f * imageAspectRatio)) };
                    rect.height -= rect.top;
                }
            }

            stage.next("start rendering", PIPER_SOURCE_LOCATION());
            DynamicArray<Spectrum<Radiance>> res(width * height, context().getAllocator());
            renderDriver->renderFrame(res, width, height, rect, transform, *tracer, *pipeline);

            stage.next("save to image", PIPER_SOURCE_LOCATION());
            auto output = opt->at("OutputFile")->get<String>();
            // TODO:move OpenEXR to ImageIO
            saveToFile(output, res, width, height);
        }
    };  // namespace Piper
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
