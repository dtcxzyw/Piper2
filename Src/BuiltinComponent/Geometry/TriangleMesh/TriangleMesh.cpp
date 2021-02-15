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
#include "../../../Interface/BuiltinComponent/Geometry.hpp"
#include "../../../Interface/BuiltinComponent/StructureParser.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#pragma warning(push, 0)
// NOTE: assimp -> Irrlicht.dll -> opengl32.dll will cause memory leak.
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
        AccelerationStructure& getAcceleration(Tracer& tracer, Accelerator& accelerator,
                                               ResourceCacheManager& cacheManager) const override {
            return *cacheManager.materialize(
                reinterpret_cast<ResourceID>(this),
                Function<SharedPtr<AccelerationStructure>>{
                    [path = mPath, &tracer, ctx = &context(), &accelerator]() -> SharedPtr<AccelerationStructure> {
                        // TODO: filesystem
                        auto importer = makeSharedPtr<Assimp::Importer>(ctx->getAllocator());
                        const auto* scene = importer->ReadFile(path.c_str(),
                                                               aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                                                   aiProcess_SortByPType | aiProcess_GenSmoothNormals |
                                                                   aiProcess_FixInfacingNormals | aiProcess_ImproveCacheLocality);

                        auto& errorHandler = ctx->getErrorHandler();

                        if(!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE)
                            errorHandler.raiseException("Failed to load scene " + path + " : " + importer->GetErrorString(),
                                                        PIPER_SOURCE_LOCATION());
                        if(scene->mNumMeshes != 1)
                            errorHandler.raiseException("Only one mesh is supported.", PIPER_SOURCE_LOCATION());
                        const auto* mesh = scene->mMeshes[0];

                        TriangleIndexedGeometryDesc desc;
                        desc.triCount = mesh->mNumFaces;
                        desc.vertCount = mesh->mNumVertices;
                        desc.index = 0;
                        auto size = 3 * sizeof(uint32_t) * mesh->mNumFaces;
                        alignTo(size, tracer.getAlignmentRequirement(AlignmentRequirement::IndexBuffer));

                        alignTo(size, tracer.getAlignmentRequirement(AlignmentRequirement::VertexBuffer));
                        desc.vertices = size;
                        size += 3 * sizeof(float) * mesh->mNumVertices;
                        alignTo(size, tracer.getAlignmentRequirement(AlignmentRequirement::VertexBuffer));

                        if(mesh->mTextureCoords[0]) {
                            alignTo(size, tracer.getAlignmentRequirement(AlignmentRequirement::TextureCoordsBuffer));
                            desc.texCoords = size;
                            size += 2 * sizeof(float) * mesh->mNumVertices;
                            alignTo(size, tracer.getAlignmentRequirement(AlignmentRequirement::TextureCoordsBuffer));
                        } else
                            desc.texCoords = invalidOffset;

                        // TODO: normal and tangent
                        desc.normal = desc.tangent = invalidOffset;

                        desc.buffer =
                            accelerator.createBuffer(size, tracer.getAlignmentRequirement(AlignmentRequirement::IndexBuffer));

                        desc.buffer->upload([ref = std::move(importer), mesh, &errorHandler, &tracer](Ptr ptr) {
                            for(Index i = 0; i < mesh->mNumFaces; ++i) {
                                if(mesh->mFaces[i].mNumIndices != 3)
                                    errorHandler.raiseException("Only triangle is supported.", PIPER_SOURCE_LOCATION());
                                memcpy(reinterpret_cast<void*>(ptr + i * 3 * sizeof(uint32_t)), mesh->mFaces[i].mIndices,
                                       sizeof(uint32_t) * 3);
                            }
                            ptr += 3 * sizeof(uint32_t) * mesh->mNumFaces;
                            alignTo(ptr, tracer.getAlignmentRequirement(AlignmentRequirement::IndexBuffer));

                            alignTo(ptr, tracer.getAlignmentRequirement(AlignmentRequirement::VertexBuffer));
                            memcpy(reinterpret_cast<void*>(ptr), mesh->mVertices, 3 * sizeof(float) * mesh->mNumVertices);
                            ptr += 3 * sizeof(float) * mesh->mNumVertices;
                            alignTo(ptr, tracer.getAlignmentRequirement(AlignmentRequirement::VertexBuffer));

                            if(mesh->mTextureCoords[0]) {
                                alignTo(ptr, tracer.getAlignmentRequirement(AlignmentRequirement::TextureCoordsBuffer));

                                if(mesh->mNumUVComponents[0] != 2)
                                    errorHandler.raiseException("Only UV channel is supported.", PIPER_SOURCE_LOCATION());
                                const auto* uv = mesh->mTextureCoords[0];
                                auto* texCoords = reinterpret_cast<Vector2<float>*>(ptr);
                                for(Index i = 0; i < mesh->mNumVertices; ++i) {
                                    texCoords[i].x = uv[i].x;
                                    texCoords[i].y = uv[i].y;
                                }

                                ptr += 2 * sizeof(float) * mesh->mNumVertices;
                                alignTo(ptr, tracer.getAlignmentRequirement(AlignmentRequirement::TextureCoordsBuffer));
                            }

                            // TODO: normal and tangent
                        });

                        return tracer.buildAcceleration({ eastl::nullopt, desc });
                    } });
        }

        [[nodiscard]] GeometryProgram materialize(const MaterializeContext&) const override {
            return {};
        }
        SampledGeometryProgram materialize(TraversalHandle traversal, const MaterializeContext& ctx) const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return {};
        }

        [[nodiscard]] Area<float> area() const override {
            context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
            return { 0.0f };
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, const CString path) : Module(context), mPath(path, context.getAllocator()) {}
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
