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
#include "../../../Interface/Infrastructure/Program.hpp"
#include "../../../Interface/Infrastructure/ResourceUtil.hpp"
#include "Shared.hpp"

namespace Piper {
    struct Bounds final {
        static constexpr auto inf = std::numeric_limits<float>::max();
        Point<Distance, FOR::Local> pMin, pMax;
        Bounds() : pMin{ { inf }, { inf }, { inf } }, pMax{ { -inf }, { -inf }, { -inf } } {}
        explicit Bounds(Point<Distance, FOR::Local> p) : pMin(p), pMax(p) {}
        void update(const Point<Distance, FOR::Local> p) noexcept {
            pMin.x.val = std::fmin(pMin.x.val, p.x.val);
            pMin.y.val = std::fmin(pMin.y.val, p.y.val);
            pMin.z.val = std::fmin(pMin.z.val, p.z.val);
            pMax.x.val = std::fmax(pMax.x.val, p.x.val);
            pMax.y.val = std::fmax(pMax.y.val, p.y.val);
            pMax.z.val = std::fmax(pMax.z.val, p.z.val);
        }
    };
    static_assert(sizeof(Bounds) == 6 * sizeof(float));

    static Bounds calcPlaneBounds(const PerPlaneData& data) {
        auto bounds = Bounds{ data.origin };
        bounds.update(data.origin + data.u);
        bounds.update(data.origin + data.v);
        bounds.update(data.origin + data.u + data.v);
        return bounds;
    }

    // TODO: use 2D parameters
    class Plane final : public Geometry {
    private:
        String mKernelPath;
        DynamicArray<PerPlaneData> mPlanes;
        DynamicArray<Dimensionless<float>> mPDF, mCDF;
        DynamicArray<Bounds> mBounds;
        Area<float> mArea;

    public:
        Plane(PiperContext& context, const SharedPtr<Config>& config, String kernel)
            : Geometry(context), mKernelPath(std::move(kernel)), mPlanes{ context.getAllocator() },
              mPDF{ context.getAllocator() }, mCDF{ context.getAllocator() }, mBounds{ context.getAllocator() }, mArea{ 0.0f } {
            const auto& planes = config->at("Primitives")->viewAsArray();
            mPlanes.reserve(planes.size());
            mPDF.reserve(planes.size());
            mCDF.reserve(planes.size());
            mBounds.reserve(planes.size());
            const auto select = [](const Vector<Distance, FOR::Local>& det) -> uint32_t {
                const float absVal[3] = { std::fabs(det.x.val), std::fabs(det.y.val), std::fabs(det.z.val) };
                if(absVal[0] >= absVal[1] && absVal[0] >= absVal[2])
                    return 0;
                return absVal[1] >= absVal[0] && absVal[1] >= absVal[2] ? 1 : 2;
            };
            for(auto&& plane : planes) {
                const auto u = parseVector<Distance, FOR::Local>(plane->at("U"));
                const auto v = parseVector<Distance, FOR::Local>(plane->at("V"));
                const auto det3 = cross(u, v);
                const auto area = length(det3);
                // TODO:fix unit
                mArea = mArea + Area<float>{ area.val };
                mPlanes.push_back({ parsePoint<Distance, FOR::Local>(plane->at("Origin")), u, v,
                                    Normal<float, FOR::Local>{ det3 / area, Unsafe{} }, Normal<float, FOR::Local>{ u },
                                    select(det3) });
                // TODO:use accelerator
                mBounds.push_back(calcPlaneBounds(mPlanes.back()));
                mCDF.push_back({ mArea.val });
                mPDF.push_back({ area.val });
            }
            for(auto& cdf : mCDF)
                cdf = cdf / Dimensionless<float>{ mArea.val };
            for(auto& pdf : mPDF)
                pdf = pdf / Dimensionless<float>{ mArea.val };
        }
        AccelerationStructure& getAcceleration(Tracer& tracer, Accelerator& accelerator,
                                               ResourceCacheManager& cacheManager) const override {
            return *cacheManager.materialize(
                reinterpret_cast<ResourceID>(this), Function<SharedPtr<AccelerationStructure>>{ [&] {
                    CustomGeometryDesc desc;
                    desc.count = static_cast<uint32_t>(mPlanes.size());

                    const auto alignment = tracer.getAlignmentRequirement(AlignmentRequirement::BoundsBuffer);
                    auto size = sizeof(Bounds) * mBounds.size();
                    alignTo(size, alignment);

                    // TODO: reduce copy
                    desc.bounds = accelerator.createBuffer(size, alignment, [bounds = mBounds](const Ptr ptr) {
                        memcpy(reinterpret_cast<void*>(ptr), bounds.data(), sizeof(Bounds) * bounds.size());
                    });

                    return tracer.buildAcceleration({ eastl::nullopt, desc });
                } });
        }
        [[nodiscard]] GeometryProgram materialize(const MaterializeContext& ctx) const override {
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            // TODO: better interface
            // TODO: concurrency
            auto linkable = PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();

            // TODO: reduce copy
            auto planeData = ctx.accelerator.createBuffer(
                sizeof(PerPlaneData) * mPlanes.size(), alignof(PerPlaneData), [planes = mPlanes](const Ptr ptr) {
                    memcpy(reinterpret_cast<void*>(ptr), planes.data(), sizeof(PerPlaneData) * planes.size());
                });
            GeometryProgram prog;
            prog.payload = packSBTPayload(context().getAllocator(), PlaneData{ ctx.registerResource(planeData) });
            prog.surface = ctx.tracer.buildProgram(linkable, "planeSurface");
            prog.intersect = ctx.tracer.buildProgram(linkable, "planeIntersect");
            prog.occlude = ctx.tracer.buildProgram(linkable, "planeOcclude");

            return prog;
        }

        SampledGeometryProgram materialize(const TraversalHandle traversal, const MaterializeContext& ctx) const override {

            // TODO: concurrency
            auto pitu = context().getPITUManager().loadPITU(mKernelPath);
            auto linkable = PIPER_FUTURE_CALL(pitu, generateLinkable)(ctx.accelerator.getSupportedLinkableFormat()).getSync();

            // TODO: reuse buffer by caching
            // TODO: reduce copy
            auto planeData = ctx.accelerator.createBuffer(
                sizeof(PerPlaneData) * mPlanes.size(), alignof(PerPlaneData), [planes = mPlanes](const Ptr ptr) {
                    memcpy(reinterpret_cast<void*>(ptr), planes.data(), sizeof(PerPlaneData) * planes.size());
                });

            // TODO: reduce copy
            auto cdfData = ctx.accelerator.createBuffer(
                sizeof(Dimensionless<float>) * mCDF.size(), alignof(Dimensionless<float>), [cdf = mCDF](const Ptr ptr) {
                    memcpy(reinterpret_cast<void*>(ptr), cdf.data(), sizeof(Dimensionless<float>) * cdf.size());
                });

            // TODO: reduce copy
            auto pdfData = ctx.accelerator.createBuffer(
                sizeof(Dimensionless<float>) * mPDF.size(), alignof(Dimensionless<float>), [pdf = mPDF](const Ptr ptr) {
                    memcpy(reinterpret_cast<void*>(ptr), pdf.data(), sizeof(Dimensionless<float>) * pdf.size());
                });

            SampledGeometryProgram prog;
            prog.sample = ctx.tracer.buildProgram(linkable, "planeSample");
            prog.payload = packSBTPayload(context().getAllocator(),
                                          CDFData{
                                              ctx.registerResource(std::move(planeData)),
                                              traversal,
                                              ctx.registerResource(std::move(cdfData)),
                                              ctx.registerResource(std::move(pdfData)),
                                              Dimensionless<float>{ 1.0f / mArea.val },
                                              static_cast<uint32_t>(mCDF.size()),
                                          });
            return prog;
        }

        [[nodiscard]] Area<float> area() const override {
            return mArea;
        }
    };
    class ModuleImpl final : public Module {
    private:
        String mKernelPath;

    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        explicit ModuleImpl(PiperContext& context, const CString path)
            : Module(context), mKernelPath(path, context.getAllocator()) {
            mKernelPath += "/Kernel.bc";
        }
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Plane") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<Plane>(context(), config, mKernelPath)));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
