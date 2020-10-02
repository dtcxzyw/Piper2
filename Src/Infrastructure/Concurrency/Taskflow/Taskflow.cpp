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
#include "../../../Interface/Infrastructure/Module.hpp"
#include "../../../PiperAPI.hpp"
#include "../../../PiperContext.hpp"
#include "../../../STL/Optional.hpp"
#include <new>
#include <taskflow/taskflow.hpp>

using namespace std::chrono_literals;

namespace Piper {
    struct FutureContext {
        tf::Taskflow flow;
        std::future<void> future;
    };

    class FutureStorage final : public FutureImpl {
    private:
        void* mPtr;
        ContextHandle mHandle;

        Optional<FutureContext> mFuture;

        void* alloc(PiperContext& context, const size_t size) {
            if(size)
                return reinterpret_cast<void*>(context.getAllocator().alloc(size));
            return nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size, const bool ready, const ContextHandle handle)
            : FutureImpl(context), mPtr(alloc(context, size)), mFuture(eastl::nullopt), mHandle(handle) {
            if(!ready)
                mFuture.emplace();
        }
        FutureContext& getFutureContext() {
            return mFuture.value();
        }
        void precede(tf::Taskflow& flow, tf::Task& task) {
            if(mFuture.has_value())
                flow.composed_of(mFuture.value().flow).precede(task);
        }
        bool ready() const noexcept override {
            return !mFuture.has_value() || mFuture.value().future.wait_for(0ns) == std::future_status::ready;
        }

        void wait() const override {
            if(mFuture.has_value())
                mFuture.value().future.wait();
        }

        void* storage() const override {
            return mPtr;
        }

        ContextHandle getContextHandle() const override {
            return mHandle;
        }
    };
    class SchedulerTaskflow final : public Scheduler {
    private:
        tf::Executor mExecutor;

    public:
        PIPER_INTERFACE_CONSTRUCT(SchedulerTaskflow, Scheduler)
        void spawnImpl(const Function<void>& func, const Span<const SharedObject<FutureImpl>>& dependencies,
                       const SharedObject<FutureImpl>& res) override {
            auto&& ctx = dynamic_cast<FutureStorage*>(res.get())->getFutureContext();
            auto task = ctx.flow.emplace(func);
            for(auto&& dep : dependencies) {
                if(getContextHandle() == dep->getContextHandle())
                    dynamic_cast<FutureStorage*>(dep.get())->precede(ctx.flow, task);
                else
                    ctx.flow.emplace([dep] { dep->wait(); }).precede(task);
            }
            ctx.future = mExecutor.run(ctx.flow);
        }
        SharedObject<FutureImpl> newFutureImpl(const size_t size, const bool ready) override {
            return makeSharedPtr<FutureStorage>(context().getAllocator(), context(), size, ready,
                                                reinterpret_cast<ContextHandle>(this));
        }
        void waitAll() noexcept override {
            mExecutor.wait_for_all();
        }
        ContextHandle getContextHandle() const override {
            return reinterpret_cast<ContextHandle>(this);
        }
    };
    class ModuleImpl final : public Module {
    public:
        PIPER_INTERFACE_CONSTRUCT(ModuleImpl, Module)
        Future<SharedObject<Object>> newInstance(const StringView& classID, const SharedObject<Config>& config,
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
