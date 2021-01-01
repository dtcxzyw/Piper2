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
#include "../../../Interface/BuiltinComponent/Geometry.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#pragma warning(push, 0)
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/vector3.h>
#pragma warning(pop)

namespace Piper {
    class TriangleMesh final : public Geometry {
    private:
        String mPath;

    public:
        TriangleMesh(PiperContext& context, const SharedPtr<Config>& config)
            : Geometry(context), mPath(config->at("Path")->get<String>()) {}
        AccelerationStructure& getAcceleration(Tracer& tracer) const override {
            const auto res = tracer.getCacheManager().materialize(
                reinterpret_cast<ResourceID>(this),
                Function<SharedPtr<AccelerationStructure>>{
                    [path = mPath, &tracer, ctx = &context()]() -> SharedPtr<AccelerationStructure> {
                        // TODO:filesystem
                        Assimp::Importer importer;
                        const auto* scene =
                            importer.ReadFile(path.c_str(),
                                              aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType |
                                                  aiProcess_GenSmoothNormals  |
                                                  aiProcess_FixInfacingNormals | aiProcess_ImproveCacheLocality);

                        auto& errorHandler = ctx->getErrorHandler();

                        if(!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE)
                            errorHandler.raiseException("Failed to load scene " + path, PIPER_SOURCE_LOCATION());
                        if(scene->mNumMeshes != 1)
                            errorHandler.raiseException("Only one mesh is supported.", PIPER_SOURCE_LOCATION());
                        const auto* mesh = scene->mMeshes[0];

                        TriangleIndexedGeometryDesc desc{};
                        DynamicArray<uint32_t> index(ctx->getAllocator());
                        index.resize(3 * mesh->mNumFaces);
                        for(Index i = 0; i < mesh->mNumFaces; ++i) {
                            if(mesh->mFaces[i].mNumIndices != 3)
                                errorHandler.raiseException("Only triangle is supported.", PIPER_SOURCE_LOCATION());
                            memcpy(index.data() + i * 3, mesh->mFaces[i].mIndices, sizeof(uint32_t) * 3);
                        }

                        desc.index = reinterpret_cast<Ptr>(index.data());
                        desc.transform.reset();
                        desc.triCount = mesh->mNumFaces;
                        desc.vertCount = mesh->mNumVertices;
                        desc.vertices = reinterpret_cast<Ptr>(mesh->mVertices);

                        DynamicArray<Vector2<float>> texCoords(ctx->getAllocator());
                        if(mesh->mTextureCoords[0]) {
                            if(mesh->mNumUVComponents[0] != 2)
                                errorHandler.raiseException("Only UV channel is supported.", PIPER_SOURCE_LOCATION());
                            const auto* uv = mesh->mTextureCoords[0];
                            texCoords.resize(mesh->mNumVertices);
                            for(Index i = 0; i < mesh->mNumVertices; ++i) {
                                texCoords[i].x = uv[i].x;
                                texCoords[i].y = uv[i].y;
                            }
                            desc.texCoords = reinterpret_cast<Ptr>(texCoords.data());
                        }

                        // TODO:normal and tangent
                        desc.normal = desc.tangent = 0;

                        auto res = tracer.buildAcceleration({ PrimitiveShapeType::TriangleIndexed, { desc } });
                        // TODO:RAII
                        importer.FreeScene();
                        return std::move(res);
                    } });
            return *res;
        }
        GeometryProgram materialize(Tracer&, ResourceHolder&, const CallSiteRegister&) const override {
            return {};
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path) : Module(context), mPath(path, context.getAllocator()) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "TriangleMesh") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<TriangleMesh>(context(), config)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
