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
#include "Interface/BuiltinComponent/Environment.hpp"
#include "Interface/BuiltinComponent/Geometry.hpp"
#include "Interface/BuiltinComponent/Integrator.hpp"
#include "Interface/BuiltinComponent/Light.hpp"
#include "Interface/BuiltinComponent/RenderDriver.hpp"
#include "Interface/BuiltinComponent/Sampler.hpp"
#include "Interface/BuiltinComponent/Sensor.hpp"
#include "Interface/BuiltinComponent/Surface.hpp"
#include "Interface/BuiltinComponent/Tracer.hpp"
#include "Interface/Infrastructure/ErrorHandler.hpp"
#include "Interface/Infrastructure/Module.hpp"
#include "Interface/Infrastructure/Operator.hpp"
#include "Kernel/Protocol.hpp"
#pragma warning(push)
#define _CRT_SECURE_NO_WARNINGS
#include <OpenEXR/ImfRgbaFile.h>
#undef _CRT_SECURE_NO_WARNINGS
#pragma warning(pop)

namespace Piper {
    enum class FitMode { Fill, OverScan };
    FitMode str2FitMode(const String& mode) {
        if(mode == "Fill")
            return FitMode::Fill;
        if(mode == "OverScan")
            return FitMode::OverScan;
        throw;
    }

    class Render final : public Operator {
    private:
        static auto parseTransform(const SharedPtr<Config>& config) {
            Transform<Distance, FOR::Local, FOR::World> transform;
            const auto& arr = config->viewAsArray();
            if(arr.size() != 12)
                throw;
            for(uint32_t i = 0; i < 3; ++i)
                for(uint32_t j = 0; j < 4; ++j) {
                    transform.A2B[i][j].val = static_cast<float>(arr[i * 4 + j]->get<double>());
                }
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
            switch(config->type()) {
                case NodeType::Array: {
                    auto sub = config->viewAsArray();
                    DynamicArray<NodeInstanceDesc> subNodes(context().getAllocator());
                    for(auto&& inst : sub) {
                        NodeInstanceDesc desc;
                        desc.node = buildScene(tracer, inst->at("Node"));
                        auto&& attr = inst->viewAsObject();
                        auto iter = attr.find(String{ "Transform", context().getAllocator() });
                        if(iter != attr.cend())
                            desc.transform = parseTransform(iter->second);
                    }
                    return tracer.buildNode(subNodes);
                }
                case NodeType::Object: {
                    GSMInstanceDesc desc;
                    desc.surface = syncLoad<Surface>(config->at("Surface"));
                    desc.geometry = syncLoad<Geometry>(config->at("Geometry"));
                    auto&& attr = config->viewAsObject();
                    const auto iter = attr.find(String{ "Transform", context().getAllocator() });
                    if(iter != attr.cend())
                        desc.transform = parseTransform(iter->second);
                    return tracer.buildNode(desc);
                }
                default:
                    throw;
            }
        }

        UniqueObject<Pipeline> buildPipeline(Tracer& tracer, RenderDriver& renderDriver, const SharedPtr<Config>& config,
                                             uint32_t width, uint32_t height, float& ratio) {
            // TODO:Asset
            const auto sensor = syncLoad<Sensor>(config->at("Sensor"));
            ratio = sensor->getAspectRatio();
            const auto light = syncLoad<Light>(config->at("Light"));
            const auto environment = syncLoad<Environment>(config->at("Environment"));
            const auto integrator = syncLoad<Integrator>(config->at("Integrator"));
            const auto node = buildScene(tracer, config->at("Scene"));
            const auto& attr = config->viewAsObject();
            const auto samplerDesc = attr.find(String{ "Sampler", context().getAllocator() });
            const auto sampler = samplerDesc != attr.cend() ? syncLoad<Sampler>(samplerDesc->second) : SharedPtr<Sampler>{};
            return tracer.buildPipeline(node, *sensor, *environment, *integrator, renderDriver, *light, sampler.get(), width,
                                        height);
        }
        void saveToFile(const String& dest, const DynamicArray<Spectrum<Radiance>>& res, const uint32_t width,
                        const uint32_t height) const {
            DynamicArray<Imf::Rgba> rgba(res.size(), context().getAllocator());

            eastl::transform(res.cbegin(), res.cend(), rgba.begin(), [](const Spectrum<Radiance> rad) {
                return Imf::Rgba{ rad.r.val, rad.g.val, rad.b.val };
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
            // TODO:set workspace
            auto sizeDesc = opt->at("ImageSize")->viewAsArray();
            if(sizeDesc.size() != 2)
                throw;
            auto width = static_cast<uint32_t>(sizeDesc[0]->get<uintmax_t>());
            auto height = static_cast<uint32_t>(sizeDesc[1]->get<uintmax_t>());
            auto scenePath = opt->at("SceneDesc")->get<String>();
            auto parserID = opt->at("SceneParser")->get<String>();
            auto fitMode = str2FitMode(opt->at("FitMode")->get<String>());

            // TODO:concurrency
            auto parserFuture = context().getModuleLoader().newInstance(parserID, nullptr);
            parserFuture.wait();
            auto parser = eastl::dynamic_shared_pointer_cast<ConfigSerializer>(parserFuture.get());

            auto scene = parser->deserialize(scenePath);
            auto tracer = syncLoad<Tracer>(opt->at("Tracer"));
            auto renderDriver = syncLoad<RenderDriver>(scene->at("RenderDriver"));

            float deviceAspectRatio = -1.0f;
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

            DynamicArray<Spectrum<Radiance>> res(width * height, context().getAllocator());
            renderDriver->renderFrame(res, width, height, rect, transform, *tracer, *pipeline);

            auto output = opt->at("OutputFile")->get<String>();
            // TODO:move OpenEXR to ImageIO
            saveToFile(output, res, width, height);
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
