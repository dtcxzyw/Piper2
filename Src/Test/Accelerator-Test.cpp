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

#include "../Interface/Infrastructure/Accelerator.hpp"
#include "../Interface/Infrastructure/Config.hpp"
#include "../Interface/Infrastructure/Logger.hpp"
#include "../Interface/Infrastructure/Module.hpp"
#include "../Interface/Infrastructure/Program.hpp"
#include "TestEnvironment.hpp"
float MKcos(float x) noexcept {
    return cos(x);
}

#include "conv.hpp"
#include <atomic>
#include <random>

using namespace std::chrono_literals;

void convolutionTest(Piper::PiperContext& context, const Piper::SharedPtr<Piper::Accelerator>& accelerator,
                     const Piper::SharedPtr<Piper::PITUManager>& manager) {
    auto& logger = context.getLogger();

    using Clock = std::chrono::high_resolution_clock;
    std::mt19937_64 RNG(Clock::now().time_since_epoch().count());
    std::uniform_real_distribution<Float> URD{ 0.0f, 1.0f };
    // Convolution
    constexpr uint32_t width = 4096, height = 2160, kernelSize = 63, count = width * height;
    auto X = Piper::makeSharedPtr<Piper::DynamicArray<Float>>(context.getAllocator(), count, context.getAllocator());
    std::generate(X->begin(), X->end(), [&] { return URD(RNG); });
    auto Y =
        Piper::makeSharedPtr<Piper::DynamicArray<Float>>(context.getAllocator(), kernelSize * kernelSize, context.getAllocator());
    std::generate(Y->begin(), Y->end(), [&] { return URD(RNG); });

    auto& scheduler = context.getScheduler();
    auto devX = accelerator->createBuffer(count * sizeof(Float), 64);
    devX->upload(scheduler.value(Piper::DataHolder{ X, X->data() }));
    auto devY = accelerator->createBuffer(Y->size() * sizeof(Float), 64);
    devY->upload(scheduler.value(Piper::DataHolder{ Y, Y->data() }));
    auto devZ = accelerator->createBuffer(count * sizeof(Float), 64);

    // TODO:concurrency
    auto conv = manager->loadPITU("conv.bc");
    conv.wait();

    auto linkable = conv->generateLinkable(accelerator->getSupportedLinkableFormat());
    auto kernel = accelerator->compileKernel(Piper::Span<Piper::Future<Piper::LinkableProgram>>{ &linkable, 1 }, "conv");
    auto payload = accelerator->createPayload(Piper::InputResource{ devX->ref() }, Piper::InputResource{ devY->ref() },
                                              Piper::OutputResource{ devZ->ref() }, width, height, kernelSize);

    // for timing
    accelerator->available(devX->ref()).wait();
    accelerator->available(devY->ref()).wait();
    accelerator->available(devZ->ref()).wait();
    kernel.wait();

    if(logger.allow(Piper::LogLevel::Info))
        logger.record(Piper::LogLevel::Info, "computation start", PIPER_SOURCE_LOCATION());

    auto beg = Clock::now();
    accelerator->runKernel(count, kernel, payload);
    auto dataZ = devZ->download();
    dataZ.wait();
    auto end = Clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - beg).count();
    if(logger.allow(Piper::LogLevel::Debug))
        logger.record(Piper::LogLevel::Debug, "Duration (Accelerator) : " + Piper::toString(context.getAllocator(), dur) + " ms",
                      PIPER_SOURCE_LOCATION());

    Piper::DynamicArray<Float> standard(count, context.getAllocator());
    auto xp = X->data(), yp = Y->data(), zp = standard.data();

    beg = Clock::now();
    for(uint32_t i = 0; i < count; ++i)
        ::conv(i, xp, yp, zp, width, height, kernelSize);
    end = Clock::now();

    dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - beg).count();
    if(logger.allow(Piper::LogLevel::Debug))
        logger.record(Piper::LogLevel::Debug, "Duration (Native)  : " + Piper::toString(context.getAllocator(), dur) + " ms",
                      PIPER_SOURCE_LOCATION());

    auto Z = reinterpret_cast<const Float*>(dataZ->data());
    for(Piper::Index i = 0; i < count; ++i)
        ASSERT_FLOAT_EQ(Z[i], standard[i]);
}
void generalAcceleratorTest(Piper::PiperContext& context, const Piper::SharedPtr<Piper::Accelerator>& accelerator,
                            const Piper::SharedPtr<Piper::PITUManager>& manager) {
    convolutionTest(context, accelerator, manager);
}

TEST_F(PiperCoreEnvironment, LLVM_CPU) {
    /*
    auto allocator =
        context->getModuleLoader()
            .newInstance("Piper.Infrastructure.JemallocAllocator.Allocator", Piper::makeSharedObject<Piper::Config>(*context))
            .get();
    contextOwner->setAllocator(eastl::dynamic_shared_pointer_cast<Piper::Allocator>(allocator));
    */
    auto scheduler = context->getModuleLoader().newInstance("Piper.Infrastructure.Squirrel.Scheduler", nullptr);
    scheduler.wait();
    contextOwner->setScheduler(eastl::dynamic_shared_pointer_cast<Piper::Scheduler>(scheduler.get()));
    auto accelerator = context->getModuleLoader().newInstance("Piper.Infrastructure.Parallel.Accelerator", nullptr);
    auto manager = context->getModuleLoader().newInstance("Piper.Infrastructure.LLVMIR.LLVMIRManager", nullptr);
    accelerator.wait();
    manager.wait();
    generalAcceleratorTest(*context, eastl::dynamic_shared_pointer_cast<Piper::Accelerator>(accelerator.get()),
                           eastl::dynamic_shared_pointer_cast<Piper::PITUManager>(manager.get()));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
