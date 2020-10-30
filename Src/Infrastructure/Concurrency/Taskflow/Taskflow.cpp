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
#include "../../../Interface/Infrastructure/Allocator.hpp"
#include "../../../Interface/Infrastructure/Logger.hpp"
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include "../../../STL/Optional.hpp"
#include "../../../STL/UniquePtr.hpp"
#include <new>
// TODO:remove tf::taskflow::ready
#include <taskflow/taskflow.hpp>

using namespace std::chrono_literals;

namespace Piper {
    struct FutureContext {
        tf::Taskflow flow;
        std::future<void> future;
    };

    static bool ready(const FutureContext& context) {
        return context.flow.ready() && context.future.wait_for(0ns) == std::future_status::ready;
    }

    static void wait(const FutureContext& context) {
        context.future.wait();
        while(!context.flow.ready())
            std::this_thread::yield();
    }

    class FutureStorage final : public FutureImpl {
    private:
        void* mPtr;
        Optional<FutureContext> mFuture;
        mutable bool mFastReady;

        void* alloc(const size_t size) const {
            return size ? reinterpret_cast<void*>(context().getAllocator().alloc(size)) : nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size, const bool ready)
            : FutureImpl(context), mPtr(alloc(size)), mFuture(eastl::nullopt), mFastReady(ready) {
            if(!ready)
                mFuture.emplace();
        }
        FutureContext& getFutureContext() {
            return mFuture.value();
        }
        bool fastReady() const noexcept override {
            return mFastReady;
        }
        bool ready() const noexcept override {
            if(!mFastReady && (!mFuture.has_value() || Piper::ready(mFuture.value())))
                mFastReady = true;
            return mFastReady;
        }

        void wait() const override {
            if(mFuture.has_value())
                Piper::wait(mFuture.value());
        }

        const void* storage() const override {
            return mPtr;
        }

        // TODO:formal check
        ~FutureStorage() noexcept {
            if(!ready())
                throw;
        }
    };

    class SchedulerTaskflow final : public Scheduler {
    private:
        tf::Executor mExecutor;

        void commit(FutureContext& ctx, const tf::Task& task, const Span<const SharedPtr<FutureImpl>>& dependencies) {
            auto src = ctx.flow.placeholder();
            // TODO:check overhead
            for(auto&& dep : dependencies) {
                if(!dep || dep->fastReady())
                    continue;

                auto cond = ctx.flow.emplace([dep] { return dep->ready(); });
                auto node = ctx.flow.placeholder();
                // TODO:remove node
                cond.precede(cond, node);
                node.precede(task);
                src.precede(cond);
            }
            ctx.future = mExecutor.run(ctx.flow);
        }

    public:
        // TODO:thread num from config
        // TODO:spring worker (alloc 2*hardware_concurrency sleep half)
        explicit SchedulerTaskflow(PiperContext& context) : Scheduler(context), mExecutor(std::thread::hardware_concurrency()) {
            auto&& logger = context.getLogger();
            if(logger.allow(LogLevel::Info))
                logger.record(LogLevel::Info, "Taskflow workers : " + toString(context.getAllocator(), mExecutor.num_workers()),
                              PIPER_SOURCE_LOCATION());
        }
        void spawnImpl(Optional<Closure<>> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                       const SharedPtr<FutureImpl>& res) override {
            auto&& ctx = dynamic_cast<FutureStorage*>(res.get())->getFutureContext();
            auto task = (func.has_value() ? ctx.flow.emplace(std::move(func.value())) : ctx.flow.placeholder());
            commit(ctx, task, dependencies);
        }
        void parallelForImpl(const uint32_t n, Closure<uint32_t> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                             const SharedPtr<FutureImpl>& res) override {
            auto&& ctx = dynamic_cast<FutureStorage*>(res.get())->getFutureContext();
            // TODO:performance
            auto worker = static_cast<uint32_t>(mExecutor.num_workers());
            auto chunkSize = std::max(1U, n / (worker * 5));
            // auto task =
            //    ctx.flow.for_each_index_static(static_cast<uint32_t>(0), n, static_cast<uint32_t>(1), std::move(func),
            //    chunkSize);
            auto call = makeSharedPtr<Closure<uint32_t>>(context().getAllocator(), std::move(func));
            // TODO:remove node
            auto node = ctx.flow.placeholder();
            for(uint32_t beg = 0; beg < n; beg += chunkSize) {
                auto end = std::min(beg + chunkSize, n);
                auto task = ctx.flow.emplace([call, beg, end] {
                    for(uint32_t i = beg; i < end; ++i)
                        (*call)(i);
                });
                node.precede(task);
            }
            commit(ctx, node, dependencies);
        }
        SharedPtr<FutureImpl> newFutureImpl(const size_t size, const bool ready) override {
            return makeSharedObject<FutureStorage>(context(), size, ready);
        }
    };
    class ModuleImpl final : public Module {
    public:
        ModuleImpl(PiperContext& context, const char*) : Module(context) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Scheduler") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<SchedulerTaskflow>(context())));
            }
            throw;
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
