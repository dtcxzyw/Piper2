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

#include "../Interface/Infrastructure/Accelerator.hpp"
#include "../Interface/Infrastructure/Config.hpp"
#include "../Interface/Infrastructure/Logger.hpp"
#include "../Interface/Infrastructure/Module.hpp"
#include "../Interface/Infrastructure/Program.hpp"
#include "TestEnvironment.hpp"

#include "conv.hpp"
#include <atomic>
#include <random>

using namespace std::chrono_literals;

void convolutionTest(Piper::PiperContext& context, const Piper::SharedPtr<Piper::Accelerator>& accelerator) {
    auto& logger = context.getLogger();

    using Clock = std::chrono::high_resolution_clock;
    std::mt19937_64 RNG(Clock::now().time_since_epoch().count());
    std::uniform_real_distribution<Float> URD{ 0.0f, 1.0f };
    // Convolution
    constexpr uint32_t width = 4096, height = 2160, kernelSize = 63, count = width * height;
    Piper::DynamicArray<Float> X{ count, context.getAllocator() };
    std::generate(X.begin(), X.end(), [&] { return URD(RNG); });
    Piper::DynamicArray<Float> Y{ kernelSize * kernelSize, context.getAllocator() };
    std::generate(Y.begin(), Y.end(), [&] { return URD(RNG); });

    auto beg = Clock::now();

    auto devX = accelerator->createBuffer(count * sizeof(Float), 64);
    devX->upload([&X](const Piper::Ptr ptr) {
        // copy for computation without accelerator
        memcpy(reinterpret_cast<void*>(ptr), X.data(), X.size() * sizeof(Float));
    });
    auto devY = accelerator->createBuffer(Y.size() * sizeof(Float), 64);
    devY->upload([&Y](const Piper::Ptr ptr) {
        // copy for computation without accelerator
        memcpy(reinterpret_cast<void*>(ptr), Y.data(), Y.size() * sizeof(Float));
    });
    const auto devZ = accelerator->createBuffer(count * sizeof(Float), 64);

    auto conv = context.getPITUManager().loadPITU("conv.bc");
    auto linkable = PIPER_FUTURE_CALL(conv, generateLinkable)(accelerator->getSupportedLinkableFormat());

    // TODO:concurrency
    linkable.wait();

    auto kernel = accelerator->compileKernel<uint32_t, uint32_t, uint32_t>(
        Piper::Span<Piper::LinkableProgram>{ &linkable.getUnsafe(), 1 }, "conv");
    // TODO:better interface
    Piper::DynamicArray<Piper::ResourceView> resources{ context.getAllocator() };
    resources.push_back({ devX, Piper::ResourceAccessMode::ReadOnly });
    resources.push_back({ devY, Piper::ResourceAccessMode::ReadOnly });
    resources.push_back({ devZ, Piper::ResourceAccessMode::ReadWrite });

    auto _ = accelerator->launchKernel(Piper::Dim3{ width, 1, 1 }, Piper::Dim3{ height, 1, 1 }, kernel, resources, width, height,
                                       kernelSize);

    auto dataZ = devZ->download().getSync();

    auto end = Clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - beg).count();
    if(logger.allow(Piper::LogLevel::Debug))
        logger.record(Piper::LogLevel::Debug, "Duration (Accelerator) : " + Piper::toString(context.getAllocator(), dur) + " ms",
                      PIPER_SOURCE_LOCATION());

    Piper::DynamicArray<Float> standard(count, context.getAllocator());
    const auto xp = X.data();
    const auto yp = Y.data();
    const auto zp = standard.data();

    beg = Clock::now();
    for(uint32_t i = 0; i < count; ++i)
        ::conv(i, xp, yp, zp, width, height, kernelSize);
    end = Clock::now();

    dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - beg).count();
    if(logger.allow(Piper::LogLevel::Debug))
        logger.record(Piper::LogLevel::Debug, "Duration (Native)  : " + Piper::toString(context.getAllocator(), dur) + " ms",
                      PIPER_SOURCE_LOCATION());

    const auto* Z = reinterpret_cast<const Float*>(dataZ.data());
    for(Piper::Index i = 0; i < count; ++i)
        ASSERT_FLOAT_EQ(Z[i], standard[i]);
}

void generalAcceleratorTest(Piper::PiperContext& context, const Piper::SharedPtr<Piper::Accelerator>& accelerator) {
    convolutionTest(context, accelerator);
}

TEST_F(PiperCoreEnvironment, LLVM_CPU) {
    /*
    auto allocator =
        context->getModuleLoader()
            .newInstance("Piper.Infrastructure.JemallocAllocator.Allocator", Piper::makeSharedObject<Piper::Config>(*context))
            .get();
    contextOwner->setAllocator(eastl::dynamic_shared_pointer_cast<Piper::Allocator>(allocator));
    */
    auto& loader = context->getModuleLoader();
    contextOwner->setScheduler(loader.newInstanceT<Piper::Scheduler>("Piper.Infrastructure.Squirrel.Scheduler").getSync());
    auto manager = loader.newInstanceT<Piper::PITUManager>("Piper.Infrastructure.LLVMIR.LLVMIRManager");
    contextOwner->setPITUManager(manager.getSync());
    auto accelerator = loader.newInstanceT<Piper::Accelerator>("Piper.Infrastructure.Parallel.Accelerator");
    generalAcceleratorTest(*context, accelerator.getSync());
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
