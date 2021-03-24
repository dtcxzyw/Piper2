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

#include "../Interface/BuiltinComponent/Sampler.hpp"
#include "../Interface/Infrastructure/Config.hpp"
#include "../Interface/Infrastructure/Module.hpp"
#include "../Interface/Infrastructure/Program.hpp"
#include "TestEnvironment.hpp"

#include <random>

constexpr auto dim = 200, sampleCount = 1024, width = 1920, height = 1080;
using RandomEngine = std::mt19937_64;

struct Sample final {
    float val[dim];
    void init(const float value) {
        for(auto&& v : val)
            v = value;
    }
    void initRandom(RandomEngine& eng) {
        for(auto&& v : val)
            v = std::generate_canonical<float, std::numeric_limits<size_t>::max()>(eng);
    }
    void updateMax(const Sample& rhs) {
        for(auto idx = 0; idx < dim; ++idx)
            val[idx] = fmaxf(val[idx], rhs.val[idx]);
    }
    void updateMin(const Sample& rhs) {
        for(auto idx = 0; idx < dim; ++idx)
            val[idx] = fminf(val[idx], rhs.val[idx]);
    }
    bool operator<(const Sample& rhs) const {
        for(auto idx = 0; idx < dim; ++idx)
            if(val[idx] >= rhs.val[idx])
                return false;
        return true;
    }
};

struct Node final {
    Sample boundL, boundR;
    Node* l;
    Node* r;
    uint32_t cnt;
    uint32_t axis;
};

class KDTree final {
private:
    Piper::DynamicArray<Node> mNodes;
    Node* mRoot;

    Node* build(const Piper::Span<Sample>& span) {
        if(span.empty())
            return nullptr;
        mNodes.push_back({});
        auto& node = mNodes.back();
        node.cnt = static_cast<uint32_t>(span.size());
        if(node.cnt == 1) {
            node.boundL = node.boundR = span.front();
            node.axis = 0;
            node.l = node.r = nullptr;
        } else {
            node.boundL.init(1.5f);
            node.boundR.init(-0.5f);
            for(auto&& sample : span) {
                node.boundL.updateMin(sample);
                node.boundR.updateMax(sample);
            }
            auto maxDelta = 0.0f;
            for(auto idx = 0; idx < dim; ++idx) {
                const auto delta = node.boundR.val[idx] - node.boundL.val[idx];
                if(delta > maxDelta) {
                    maxDelta = delta;
                    node.axis = idx;
                }
            }
            std::nth_element(span.begin(), span.begin() + span.size() / 2, span.end(),
                             [axis = node.axis](const Sample& lhs, const Sample& rhs) { return lhs.val[axis] < rhs.val[axis]; });
            node.l = build(span.subspan(0, span.size() / 2));
            node.r = build(span.subspan(span.size() / 2));
        }
        return &node;
    }
    [[nodiscard]] uint32_t query(const Node& node, const uint32_t axis, const Sample& r) const {
        if(node.boundL.val[axis] < fminf(r.val[axis], node.boundR.val[axis])) {
            if(node.boundR < r)
                return node.cnt;
            uint32_t res = 0;
            if(node.l)
                res += query(*node.l, node.axis, r);
            if(node.r)
                res += query(*node.r, node.axis, r);
            return res;
        }
        return 0;
    }

public:
    explicit KDTree(Piper::PiperContext& context, Piper::DynamicArray<Sample>& samples) : mNodes{ context.getAllocator() } {
        mNodes.reserve(samples.size() * 2);
        mRoot = build({ samples.data(), samples.size() });
    }
    [[nodiscard]] uint32_t query(const Sample& r) const {
        return query(*mRoot, 0, r);
    }
};

double evalStarDiscrepancy(Piper::PiperContext& context, Piper::DynamicArray<Sample>& samples, const Piper::CString name) {
    printf("evaluate %s\n", name);
    for(auto&& sample : samples) {
        for(auto&& v : sample.val) {
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }

    constexpr auto evalSampleCount = 10000000;
    const KDTree kdTree{ context, samples };
    RandomEngine eng{ 998244353 };
    Piper::DynamicArray<std::tuple<double, Sample>> tasks{ evalSampleCount, context.getAllocator() };
    const std::uniform_int_distribution<int> axisSelector{ 0, dim - 1 };

    // TODO: better bound sampling
    for(auto idx = 0; idx < evalSampleCount; ++idx) {
        Sample r;
        r.init(1.0f);
        auto volume = 1.0;
        for(auto j = 0; j < 10; ++j) {
            const auto axis = axisSelector(eng);
            const auto v1 = std::generate_canonical<float, std::numeric_limits<size_t>::max()>(eng) * 0.5f + 0.3f;
            r.val[axis] = v1;
        }
        for(auto j = 0; j < dim; ++j)
            volume *= static_cast<double>(r.val[j]);
        tasks[idx] = { volume, r };
    }
    Piper::DynamicArray<double> results{ evalSampleCount, context.getAllocator() };

    context.getScheduler()
        .parallelFor(evalSampleCount,
                     [&](const uint32_t idx) {
                         const auto& [volume, r] = tasks[idx];
                         const auto cnt = kdTree.query(r);
                         const auto ratio = static_cast<double>(cnt) / static_cast<double>(sampleCount);
                         results[idx] = fabs(volume - ratio);
                     })
        .wait();

    const auto maxVal = *std::max_element(results.cbegin(), results.cend());

    printf("Discrepancy of %s: max %f\n", name, maxVal);
    return maxVal;
}

double evalReference(Piper::PiperContext& context) {
    static auto res = -1.0;
    if(res < 0.0) {
        RandomEngine eng{ 19260817 };  // NOLINT(cert-msc51-cpp)
        Piper::DynamicArray<Sample> samples{ sampleCount, context.getAllocator() };
        for(auto&& sample : samples)
            sample.initRandom(eng);
        res = evalStarDiscrepancy(context, samples, "std::mt19937_64");
    }
    return res;
}

template <typename T>
T& nullReference() {
    return *reinterpret_cast<T*>(nullptr);
}

struct DummyRTProgram final : Piper::RTProgram {
    Piper::LinkableProgram linkable;
    Piper::String symbol;
    DummyRTProgram(Piper::PiperContext& context, Piper::LinkableProgram linkable, Piper::String symbol)
        : RTProgram{ context }, linkable{ std::move(linkable) }, symbol{ std::move(symbol) } {}
};

class DummyTracer final : public Piper::Tracer {
private:
    Piper::Accelerator& mAccelerator;

public:
    DummyTracer(Piper::PiperContext& context, Piper::Accelerator& accelerator) : Tracer{ context }, mAccelerator{ accelerator } {}
    [[nodiscard]] Piper::Accelerator& getAccelerator() const noexcept override {
        return mAccelerator;
    }
    [[nodiscard]] size_t getAlignmentRequirement(Piper::AlignmentRequirement) const noexcept override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return std::numeric_limits<size_t>::max();
    }
    [[nodiscard]] Piper::SharedPtr<Piper::Texture> generateTexture(const Piper::SharedPtr<Piper::Config>&,
                                                                   uint32_t) const override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return nullptr;
    }
    [[nodiscard]] Piper::SharedPtr<Piper::AccelerationStructure> buildAcceleration(const Piper::GeometryDesc& desc) override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return nullptr;
    }
    [[nodiscard]] Piper::SharedPtr<Piper::GSMInstance> buildGSMInstance(Piper::SharedPtr<Piper::Geometry> geometry,
                                                                        Piper::SharedPtr<Piper::Surface> surface,
                                                                        Piper::SharedPtr<Piper::Medium> medium) override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return nullptr;
    }
    [[nodiscard]] Piper::SharedPtr<Piper::Node>
    buildNode(const Piper::DynamicArray<eastl::pair<Piper::TransformInfo, eastl::shared_ptr<Piper::Node>>>& children) override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return nullptr;
    }
    [[nodiscard]] Piper::SharedPtr<Piper::Node> buildNode(const Piper::SharedPtr<Object>& object) override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return nullptr;
    }
    [[nodiscard]] Piper::UniqueObject<Piper::Pipeline>
    buildPipeline(const Piper::SharedPtr<Piper::Node>& scene, Piper::Integrator& integrator, Piper::RenderDriver& renderDriver,
                  Piper::LightSampler& lightSampler, Piper::SharedPtr<Piper::Sampler> sampler) override {
        context().getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
        return nullptr;
    }
    [[nodiscard]] Piper::SharedPtr<Piper::RTProgram> buildProgram(Piper::LinkableProgram linkable,
                                                                  Piper::String symbol) override {
        return Piper::makeSharedObject<DummyRTProgram>(context(), std::move(linkable), std::move(symbol));
    }
};

static void dummyEntry(const Piper::TaskContext) {}

static Piper::LinkableProgram prepareKernelNative(Piper::PiperContext& context) {
    Piper::Binary res{ context.getAllocator() };
    constexpr auto header = "Native";
    res.insert(res.cend(), reinterpret_cast<const std::byte*>(header), reinterpret_cast<const std::byte*>(header + 6));
    auto append = [&res](const Piper::StringView symbol, auto address) {
        res.insert(res.cend(), reinterpret_cast<const std::byte*>(symbol.data()),
                   reinterpret_cast<const std::byte*>(symbol.data() + symbol.size() + 1));
        auto func = reinterpret_cast<ptrdiff_t>(address);
        const auto* beg = reinterpret_cast<const std::byte*>(&func);
        const auto* end = beg + sizeof(func);
        res.insert(res.cend(), beg, end);
    };
    append("dummy", dummyEntry);
    return Piper::LinkableProgram{ context.getScheduler().value(res), Piper::String{ "Native", context.getAllocator() },
                                   reinterpret_cast<uint64_t>(prepareKernelNative) };
}

// TODO: test global sampling
TEST_F(PiperCoreEnvironment, SobolSampler) {
    auto& loader = context->getModuleLoader();
    contextOwner->setScheduler(loader.newInstanceT<Piper::Scheduler>("Piper.Infrastructure.Squirrel.Scheduler").getSync());

    const auto reference = evalReference(*context);

    auto manager = loader.newInstanceT<Piper::PITUManager>("Piper.Infrastructure.LLVMIR.LLVMIRManager");
    contextOwner->setPITUManager(manager.getSync());
    const auto accelerator = loader.newInstanceT<Piper::Accelerator>("Piper.Infrastructure.Parallel.Accelerator").getSync();
    const auto samplerDesc = Piper::makeSharedObject<Piper::Config>(
        *context,
        Piper::UMap<Piper::String, Piper::SharedPtr<Piper::Config>>{
            { Piper::makePair(
                  Piper::String{ "ClassID", context->getAllocator() },
                  Piper::makeSharedObject<Piper::Config>(
                      *context,
                      Piper::String{ "Piper.BuiltinComponent.ScrambledSobolSampler.Sampler", context->getAllocator() })),
              Piper::makePair(Piper::String{ "SamplesPerPixel", context->getAllocator() },
                              Piper::makeSharedObject<Piper::Config>(*context, 1000U)),
              Piper::makePair(Piper::String{ "Scramble", context->getAllocator() },
                              Piper::makeSharedObject<Piper::Config>(*context, false)) },
            0,
            {},
            {},
            context->getAllocator() });

    const auto sampler = context->getModuleLoader().newInstanceT<Piper::Sampler>(samplerDesc).getSync();
    const auto tracer = Piper::makeSharedObject<DummyTracer>(*context, *accelerator);

    const Piper::MaterializeContext ctx{
        *tracer, *accelerator, nullReference<Piper::ResourceCacheManager>(), nullReference<Piper::Profiler>(), {}, {}, {}
    };
    const auto prog = sampler->materialize(ctx);
    const auto generate = dynamic_cast<DummyRTProgram*>(prog.generate.get());
    const auto start = dynamic_cast<DummyRTProgram*>(prog.start.get());

    Piper::LinkableProgram linkable[] = { generate->linkable, start->linkable, prepareKernelNative(*context) };
    Piper::DynamicArray<Sample> samples{ sampleCount, context->getAllocator() };
    auto kernel = accelerator
                      ->compileKernel(linkable, Piper::UMap<Piper::String, Piper::String>{ context->getAllocator() },
                                      Piper::DynamicArray<Piper::String>{ context->getAllocator() })
                      .get();
    context->getErrorHandler().notImplemented(PIPER_SOURCE_LOCATION());
    /*
    const auto startFunction = static_cast<Piper::SampleStartFunc>(kernel->lookup(start->symbol));
    const auto generateFunction = static_cast<Piper::SampleGenerateFunc>(kernel->lookup(generate->symbol));
    const auto attr = sampler->generatePayload(width, height);
    EXPECT_LE(static_cast<uint32_t>(dim), attr.maxDimension);
    for(auto idx = 0; idx < sampleCount; ++idx) {
        Piper::Vector2<float> unused;
        uint64_t index;
        startFunction(attr.payload.data(), width / 2, height / 2, idx, index, unused);
        auto&& sample = samples[idx];
        for(auto i = 0; i < dim; ++i)
            generateFunction(attr.payload.data(), index, i, sample.val[i]);
    }
    */
    const auto res = evalStarDiscrepancy(*context, samples, "Piper.BuiltinComponent.ScrambledSobolSampler");
    EXPECT_LE(res, reference);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
