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
#include "../Interface/Infrastructure/Module.hpp"
#include "../Interface/Infrastructure/Program.hpp"
#include "TestEnvironment.hpp"
#include <atomic>
#include <random>

using namespace std::chrono_literals;

void generalAcceleratorTest(Piper::PiperContext& context, Piper::SharedObject<Piper::Accelerator> accelerator,
                            Piper::SharedObject<Piper::PITUManager> manager) {
    using Clock = std::chrono::high_resolution_clock;
    std::mt19937_64 RNG(Clock::now().time_since_epoch().count());
    std::uniform_real_distribution<float> URD{ 0.0f, 1.0f };
    // saxpy:Z[i]=alpha*X[i]+Y[i]
    constexpr size_t count = 100000000, repeat = 10;
    constexpr auto alpha = 5.0f;
    Piper::Vector<float> X(count, context.getAllocator()), Y(count, context.getAllocator());
    std::generate(X.begin(), X.end(), [&] { return URD(RNG); });
    std::generate(Y.begin(), Y.end(), [&] { return URD(RNG); });
    auto devX = accelerator->createBuffer(count * sizeof(float), 64);
    devX->upload(X.data());
    auto devY = accelerator->createBuffer(count * sizeof(float), 64);
    devY->upload(Y.data());
    auto devZ = accelerator->createBuffer(count * sizeof(float), 64);
    devZ->reset();

    // TODO:concurrency
    auto saxpy = manager->loadPITU("saxpy.bc").get();
    auto linkable = saxpy->generateLinkable(accelerator->getSupportedLinkableFormat());
    auto kernel = accelerator->compileKernel(
        Piper::Vector<Piper::Future<Piper::Vector<std::byte>>>{ { linkable }, context.getAllocator() }, "saxpy");
    auto params = accelerator->createParameters();

    params->appendInput(devX->ref());
    params->appendInput(devY->ref());
    params->appendAccumulate(devZ->ref());
    params->append(alpha);

    auto beg = Clock::now();
    for(size_t i = 0; i < repeat; ++i)
        accelerator->runKernel(count, kernel, params);
    auto dataZ = devZ->download().get();
    auto end = Clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - beg).count();
    if(context.getLogger().allow(Piper::LogLevel::Debug))
        context.getLogger().record(Piper::LogLevel::Debug, "Duration : " + Piper::toString(context.getAllocator(), dur) + " ms",
                                   PIPER_SOURCE_LOCATION());

    auto Z = reinterpret_cast<const float*>(dataZ.data());
    for(Piper::Index i = 0; i < count; ++i)
        ASSERT_FLOAT_EQ(Z[i], repeat * (alpha * X[i] + Y[i]));
}

TEST_F(PiperCoreEnvironment, LLVM_CPU) {
    auto scheduler = context->getModuleLoader().newInstance("Piper.Infrastructure.Taskflow.Scheduler", nullptr).get();
    contextOwner->setScheduler(eastl::dynamic_shared_pointer_cast<Piper::Scheduler>(scheduler));
    auto accelerator = context->getModuleLoader().newInstance("Piper.Infrastructure.Parallel.Accelerator", nullptr);
    auto manager = context->getModuleLoader().newInstance("Piper.Infrastructure.LLVMIR.LLVMIRManager", nullptr);
    generalAcceleratorTest(*context, eastl::dynamic_shared_pointer_cast<Piper::Accelerator>(accelerator.get()),
                           eastl::dynamic_shared_pointer_cast<Piper::PITUManager>(manager.get()));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
