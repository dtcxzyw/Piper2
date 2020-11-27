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
#include "Interface/BuiltinComponent/Sensor.hpp"
#include "Interface/BuiltinComponent/Surface.hpp"
#include "Interface/BuiltinComponent/Tracer.hpp"
#include "Interface/Infrastructure/Module.hpp"
#include "Interface/Infrastructure/Operator.hpp"
#include "Kernel/Protocol.hpp"
#pragma warning(push)
#define _CRT_SECURE_NO_WARNINGS
#include "Interface/BuiltinComponent/Sampler.hpp"

#include <OpenEXR/ImfRgbaFile.h>
#undef _CRT_SECURE_NO_WARNINGS
#pragma warning(pop)

namespace Piper {
    class Render final : public Operator {
    private:
        auto parseTransform(const SharedPtr<Config>& config) {
            Transform<Distance, FOR::Local, FOR::World> transform;
            auto arr = config->viewAsArray();
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
                    auto iter = attr.find(String{ "Transform", context().getAllocator() });
                    if(iter != attr.cend())
                        desc.transform = parseTransform(iter->second);
                    return tracer.buildNode(desc);
                }
                default:
                    throw;
            }
        }

        UniqueObject<Pipeline> buildPipeline(Tracer& tracer, RenderDriver& renderDriver, const SharedPtr<Config>& config,
                                             uint32_t width, uint32_t height) {
            // TODO:Asset
            auto sensor = syncLoad<Sensor>(config->at("Sensor"));
            auto light = syncLoad<Light>(config->at("Light"));
            auto environment = syncLoad<Environment>(config->at("Environment"));
            auto integrator = syncLoad<Integrator>(config->at("Integrator"));
            auto node = buildScene(tracer, config->at("Scene"));
            auto attr = config->viewAsObject();
            auto samplerDesc = attr.find(String{ "Sampler", context().getAllocator() });
            auto sampler = samplerDesc != attr.cend() ? syncLoad<Sampler>(samplerDesc->second) : SharedPtr<Sampler>{};
            return tracer.buildPipeline(node, *sensor, *environment, *integrator, renderDriver, *light, sampler.get(), width,
                                        height);
        }
        void saveToFile(const String& dest, const DynamicArray<Spectrum<Radiance>>& res, uint32_t width, uint32_t height) {
            DynamicArray<Imf::Rgba> rgba(res.size(), context().getAllocator());

            eastl::transform(res.cbegin(), res.cend(), rgba.begin(), [](Spectrum<Radiance> rad) {
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
            // TODO:concurrency
            auto parserFuture = context().getModuleLoader().newInstance(parserID, nullptr);
            parserFuture.wait();
            auto parser = eastl::dynamic_shared_pointer_cast<ConfigSerializer>(parserFuture.get());

            auto scene = parser->deserialize(scenePath);

            auto tracer = syncLoad<Tracer>(opt->at("Tracer"));

            auto renderDriver = syncLoad<RenderDriver>(scene->at("RenderDriver"));

            auto pipeline = buildPipeline(*tracer, *renderDriver, scene, width, height);
            DynamicArray<Spectrum<Radiance>> res(width * height, context().getAllocator());
            renderDriver->renderFrame(res, width, height, *tracer, *pipeline);

            auto output = opt->at("OutputFile")->get<String>();
            // TODO:remove exr
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
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
