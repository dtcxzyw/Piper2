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
#include <utility>

#include "../../../Interface/BuiltinComponent/ImageIO.hpp"
#include "../../../Interface/BuiltinComponent/Texture.hpp"
#include "../../../Interface/BuiltinComponent/TextureSampler.hpp"
#include "../../../Interface/Infrastructure/Accelerator.hpp"
#include "../../../Interface/Infrastructure/ErrorHandler.hpp"
#include "../../../Interface/Infrastructure/FileSystem.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../Interface/Infrastructure/Program.hpp"
#include "Shared.hpp"
#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma warning(pop)

namespace Piper {
    class SampledTexture final : public Texture {
    private:
        DynamicArray<std::byte> mImage;
        Data mData;
        uint32_t mChannel;
        Future<SharedPtr<PITU>> mKernel;

    public:
        SampledTexture(PiperContext& context, const std::byte* data, const ImageAttributes& attributes, const TextureWrap wrap,
                       Future<SharedPtr<PITU>> kernel)
            : Texture(context),
              mImage(data, data + attributes.width * attributes.height * attributes.channel, context.getAllocator()),
              mData{ wrap,
                     attributes.width,
                     attributes.height,
                     attributes.width * attributes.channel,
                     attributes.channel,
                     reinterpret_cast<const unsigned char*>(mImage.data()) },
              mChannel(attributes.channel), mKernel(std::move(kernel)) {}

        [[nodiscard]] uint32_t channel() const noexcept override {
            return mChannel;
        }
        TextureProgram materialize(Tracer& tracer, ResourceHolder& holder) const override {
            TextureProgram res;
            // TODO:concurrency
            const auto linkable = mKernel.getSync()->generateLinkable(tracer.getAccelerator().getSupportedLinkableFormat());
            res.sample = tracer.buildProgram(linkable, "sample");
            res.payload = packSBTPayload(context().getAllocator(), mData);
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
            return makeSharedObject<SampledTexture>(context(), image->data(), image->attributes(), wrap, mKernel);
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

    class ImageReader final : public ImageIO {
    public:
        explicit ImageReader(PiperContext& context) noexcept : ImageIO(context) {}

        [[nodiscard]] SharedPtr<Image> loadImage(const String& path, const uint32_t channel) const override {
            auto& errorHandler = context().getErrorHandler();
            auto stage = errorHandler.enterStage("load image " + path, PIPER_SOURCE_LOCATION());
            if(channel != 1 && channel != 2 && channel != 4)
                errorHandler.raiseException("Unsupported channel " + toString(context().getAllocator(), channel),
                                            PIPER_SOURCE_LOCATION());
            const auto file = context().getFileSystem().mapFile(path, FileAccessMode::Read, FileCacheHint::Sequential);
            {
                const auto image = file->map(0, file->size());
                const auto span = image->get();

                // TODO:color-space
                int x, y, srcChannel;
                ImageHolder ptr{ reinterpret_cast<std::byte*>(stbi_load_from_memory(reinterpret_cast<stbi_uc*>(span.data()),
                                                                                    static_cast<int>(span.size_bytes()), &x, &y,
                                                                                    &srcChannel, channel)) };
                if(ptr)
                    return makeSharedObject<ImageStorage>(context(), std::move(ptr), x, y, channel);
                errorHandler.raiseException(stbi_failure_reason(), PIPER_SOURCE_LOCATION());
            }
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
            if(classID == "TextureSampler") {
                // TODO:concurrency
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<SoftwareTextureSampler>(context(), mPath)));
            }
            if(classID == "ImageReader") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<ImageReader>(context())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
