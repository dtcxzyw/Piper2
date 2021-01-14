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
#include <utility>
#include "../../../Interface/BuiltinComponent/Image.hpp"
#include "../../../Interface/BuiltinComponent/Texture.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Profiler.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#include "Shared.hpp"
#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma warning(pop)

namespace Piper {
    class SampledTexture final : public Texture {
    private:
        SharedPtr<Image> mImage;
        Data mData;
        Future<SharedPtr<PITU>> mKernel;

    public:
        SampledTexture(PiperContext& context, const SharedPtr<Image>& image, const TextureWrap wrap,
                       Future<SharedPtr<PITU>> kernel)
            : Texture(context), mImage(image), mData{ wrap,
                                                      image->attributes().width,
                                                      image->attributes().height,
                                                      image->attributes().width * image->attributes().channel,
                                                      image->attributes().channel,
                                                      reinterpret_cast<const unsigned char*>(mImage->data()) },
              mKernel(std::move(kernel)) {}

        [[nodiscard]] uint32_t channel() const noexcept override {
            return mData.channel;
        }
        TextureProgram materialize(const MaterializeContext& ctx) const override {
            TextureProgram res;
            // TODO:concurrency
            const auto linkable = mKernel.getSync()->generateLinkable(ctx.tracer.getAccelerator().getSupportedLinkableFormat());
            res.sample = ctx.tracer.buildProgram(linkable, "sampleTexture");
            ctx.holder.retain(mImage);
            auto data = mData;
            static char uid;
            data.profileSample = ctx.profiler.registerDesc("Texture", "Sample Time", &uid, StatisticsType::Time);
            res.payload = packSBTPayload(context().getAllocator(), data);
            return res;
        }
    };

    class SoftwareTextureSampler final : public TextureSampler {
    private:
        // TODO:better architecture
        Future<SharedPtr<PITU>> mKernel;

    public:
        SoftwareTextureSampler(PiperContext& context, const String& path)
            : TextureSampler(context), mKernel(context.getPITUManager().loadPITU(path + "/Kernel.bc")) {}
        SharedPtr<Texture> generateTexture(const SharedPtr<Image>& image, const TextureWrap wrap) const override {
            return makeSharedObject<SampledTexture>(context(), image, wrap, mKernel);
        }
    };

    // TODO:allocator
    struct ImageDeleter {
        void operator()(void* ptr) const {
            stbi_image_free(ptr);
        }
    };
    using ImageHolder = UniquePtr<std::byte, ImageDeleter>;

    class ImageStorage final : public Image {
    private:
        ImageHolder mData;
        ImageAttributes mAttributes;

    public:
        ImageStorage(PiperContext& context, ImageHolder holder, const uint32_t width, const uint32_t height,
                     const uint32_t channel) noexcept
            : Image(context), mData(std::move(holder)), mAttributes{ width, height, channel } {}
        [[nodiscard]] const ImageAttributes& attributes() const noexcept override {
            return mAttributes;
        }

        [[nodiscard]] const std::byte* data() const noexcept override {
            return mData.get();
        }
    };

    class ModuleImpl final : public Module {
    private:
        String mPath;
        [[nodiscard]] SharedPtr<Image> loadImage(const String& path) const {
            auto& errorHandler = context().getErrorHandler();
            auto stage = errorHandler.enterStage("load image " + path, PIPER_SOURCE_LOCATION());
            const auto file = context().getFileSystem().mapFile(path, FileAccessMode::Read, FileCacheHint::Sequential);
            {
                const auto image = file->map(0, file->size());
                const auto span = image->get();

                // TODO:color-space
                int x, y, srcChannel;
                if(!stbi_info_from_memory(reinterpret_cast<stbi_uc*>(span.data()), static_cast<int>(span.size_bytes()), &x, &y,
                                          &srcChannel))
                    errorHandler.raiseException(stbi_failure_reason(), PIPER_SOURCE_LOCATION());

                auto desired = srcChannel;
                if(srcChannel == STBI_rgb)
                    desired = STBI_rgb_alpha;

                ImageHolder ptr{ reinterpret_cast<std::byte*>(stbi_load_from_memory(reinterpret_cast<stbi_uc*>(span.data()),
                                                                                    static_cast<int>(span.size_bytes()), &x, &y,
                                                                                    &srcChannel, desired)) };
                if(ptr)
                    return makeSharedObject<ImageStorage>(context(), std::move(ptr), x, y, desired);
                errorHandler.raiseException(stbi_failure_reason(), PIPER_SOURCE_LOCATION());
            }
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, CString path) : Module(context), mPath(path, context.getAllocator()) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "TextureSampler") {
                // TODO:concurrency
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<SoftwareTextureSampler>(context(), mPath)));
            }
            if(classID == "Image") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(loadImage(config->at("Path")->get<String>())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
