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
#include "../../../STL/List.hpp"
#include <condition_variable>
#include <mutex>

using namespace std::chrono_literals;

namespace Piper {
    struct SharedContext;
    struct DelayedTask;

    static void decreaseDepCount(SharedContext& context, DelayedTask* task);

    class FutureStorage final : public FutureImpl {
    private:
        std::mutex mMutex;
        DynamicArray<DelayedTask*> mSuccessor;
        void* mPtr;
        std::atomic_uint32_t mRemain;

        void* alloc(const size_t size) const {
            return size ? reinterpret_cast<void*>(context().getAllocator().alloc(size)) : nullptr;
        }

    public:
        FutureStorage(PiperContext& context, const size_t size)
            : FutureImpl(context), mSuccessor(context.getAllocator()), mPtr(alloc(size)), mRemain(0) {}

        void setRemain(const uint32_t val) {
            mRemain = val;
        }

        bool ready() const noexcept override {
            return !mRemain;
        }
        bool fastReady() const noexcept override {
            return !mRemain;
        }
        void wait() const override {
            auto& logger = context().getLogger();
            if(logger.allow(LogLevel::Warning))
                logger.record(LogLevel::Warning, "Busy waiting of future may cause bad performance.", PIPER_SOURCE_LOCATION());
            while(mRemain)
                std::this_thread::yield();
        }
        const void* storage() const override {
            return mPtr;
        }
        // ready->true
        bool readyOrNotify(DelayedTask* successor) {
            if(mRemain) {
                std::lock_guard<std::mutex> guard{ mMutex };
                if(!mRemain)
                    return true;
                mSuccessor.emplace_back(successor);
                return false;
            }
            return true;
        }
        void finishOne(SharedContext& context) {
            if(mRemain == 0)
                throw;
            if(--mRemain)
                return;
            std::lock_guard<std::mutex> guard{ mMutex };
            for(auto& successor : mSuccessor)
                decreaseDepCount(context, successor);
            mSuccessor.clear();
            mSuccessor.shrink_to_fit();
        }
        ~FutureStorage() noexcept override {
            if(mRemain)
                context().getErrorHandler().raiseException("Not handled future", PIPER_SOURCE_LOCATION());
        }
    };

    struct Task final : private Uncopyable {
        Variant<Optional<Closure<>>, Pair<SharedPtr<Closure<uint32_t>>, uint32_t>> work;
        SharedPtr<FutureStorage> future;
        template <typename T>
        Task(T&& task, SharedPtr<FutureStorage> futureStorage) : work(std::forward<T>(task)), future(std::move(futureStorage)) {}
    };

    struct DelayedTask final {
        STLAllocator allocator;
        Task task;
        DynamicArray<SharedPtr<FutureImpl>> depRef;
        std::atomic_size_t depCount;

        DelayedTask(const STLAllocator& alloc, Task work)
            : allocator(alloc), task(std::move(work)), depRef(allocator), depCount(0) {}
    };

    struct ExternalFuture final {
        SharedPtr<FutureImpl> ref;
        DynamicArray<DelayedTask*> successor;
        ExternalFuture(SharedPtr<FutureImpl> future, STLAllocator allocator) : ref(std::move(future)), successor(allocator) {}
    };

    struct SharedContext final {
        std::mutex waitMutex;
        DynamicArray<Task> todo;
        std::recursive_mutex todoMutex;

        UMap<FutureImpl*, ExternalFuture> notifyExternalFuture;
        std::mutex NEFMutex;

        UMap<FutureImpl*, ExternalFuture> checkExternalFuture;
        std::mutex CEFMutex;

        bool run;
        std::condition_variable cv;
        explicit SharedContext(const STLAllocator& allocator)
            : todo(allocator), notifyExternalFuture(allocator), checkExternalFuture(allocator), run(true) {}
    };

    static void readyToRun(SharedContext& context, Task task) {
        std::lock_guard<std::recursive_mutex> guard{ context.todoMutex };
        const auto idx = task.work.index();
        if(idx == 1 || get<Optional<Closure<>>>(task.work).has_value()) {
            context.todo.push_back(std::move(task));
            if(idx)
                context.cv.notify_all();
            else
                context.cv.notify_one();
        } else {
            // NOTICE:finishOne->finishExternal->readyToRun
            task.future->finishOne(context);
        }
    }

    static void decreaseDepCount(SharedContext& context, DelayedTask* task) {
        if(--task->depCount)
            return;
        // remove delayed task
        auto guard = UniquePtr<DelayedTask>{ task, DefaultDeleter<DelayedTask>{ task->allocator } };
        readyToRun(context, std::move(task->task));
    }

    static void worker(SharedContext& context) {
        while(context.run) {
            // suspend
            {
                std::unique_lock<std::mutex> guard{ context.waitMutex };
                context.cv.wait(guard, [&] {
                    if(!context.run)
                        return true;
                    return !context.todo.empty() || !context.checkExternalFuture.empty();
                });
            }

            while(!(context.todo.empty() && context.checkExternalFuture.empty())) {
                if(!context.run)
                    return;
                // work
                while(!context.todo.empty()) {
                    if(!context.run)
                        return;
                    std::unique_lock<std::recursive_mutex> guard{ context.todoMutex };
                    if(context.todo.empty())
                        break;
                    using Work = Variant<Closure<>, Pair<SharedPtr<Closure<uint32_t>>, uint32_t>>;

                    auto& task = context.todo.back();
                    SharedPtr<FutureStorage> future;
                    auto acquireWork = [&] {
                        if(task.work.index()) {
                            auto& ref = get<Pair<SharedPtr<Closure<uint32_t>>, uint32_t>>(task.work);
                            --ref.second;
                            if(ref.second == 0) {
                                auto val = std::move(ref);
                                future = std::move(task.future);
                                context.todo.pop_back();
                                return Work{ std::move(val) };
                            }
                            future = task.future;
                            return Work{ ref };
                        } else {
                            auto work = Work{ std::move(get<Optional<Closure<>>>(task.work).value()) };
                            future = std::move(task.future);
                            context.todo.pop_back();
                            return std::move(work);
                        }
                    };
                    auto work = acquireWork();
                    guard.unlock();

                    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
                    switch(work.index()) {
                        case 0: {
                            get<Closure<>>(work)();
                        } break;
                        case 1: {
                            auto& ref = get<Pair<SharedPtr<Closure<uint32_t>>, uint32_t>>(work);
                            (*ref.first)(ref.second);
                        } break;
                    }

                    future->finishOne(context);
                }
                // checkFuture
                while(!context.checkExternalFuture.empty() && context.todo.empty()) {
                    if(!context.run)
                        return;
                    std::unique_lock<std::mutex> guard{ context.CEFMutex, std::try_to_lock };
                    // Another worker is checking futures
                    if(!guard.owns_lock())
                        break;
                    // ReSharper disable once CppRedundantQualifier
                    eastl::erase_if(context.checkExternalFuture, [&context](const Pair<FutureImpl*, ExternalFuture>& val) {
                        auto&& future = val.second;
                        if(!future.ref->ready())
                            return false;
                        for(auto&& successor : future.successor)
                            decreaseDepCount(context, successor);
                        return true;
                    });
                }
            }
        }
    }

    // TODO:lockfree data structure
    class SchedulerImpl final : public Scheduler {
    private:
        DynamicArray<std::thread> mThreadPool;
        SharedContext mContext;

        void emitTask(DelayedTask* delay, const Span<const SharedPtr<FutureImpl>>& dependencies) {
            // fake depCount for avoiding incomplete spawn
            ++delay->depCount;

            for(auto&& dep : dependencies) {
                if(!dep || dep->fastReady())
                    continue;
                // Perhaps future will call finishExternal before the task is complete,so we should increase depCount before
                // future->readyOrNotify
                ++delay->depCount;
                if(auto* future = dynamic_cast<FutureStorage*>(dep.get())) {
                    if(future->readyOrNotify(delay))
                        --delay->depCount;
                    else {
                        // add reference
                        delay->depRef.push_back(dep);
                    }
                } else {
                    auto appendDep = [&](std::mutex& lock, UMap<FutureImpl*, ExternalFuture>& cont) {
                        auto* key = dep.get();
                        std::lock_guard<std::mutex> guard{ lock };
                        auto iter = cont.find(key);
                        if(iter == cont.cend())
                            iter = cont.emplace(key, ExternalFuture{ dep, context().getAllocator() }).first;
                        iter->second.successor.push_back(delay);
                    };

                    if(dep->supportNotify())
                        appendDep(mContext.NEFMutex, mContext.notifyExternalFuture);
                    else
                        appendDep(mContext.CEFMutex, mContext.checkExternalFuture);
                }
            }
            // The task is complete now,decrease depCount
            decreaseDepCount(mContext, delay);
            if(!mContext.checkExternalFuture.empty())
                mContext.cv.notify_one();
        }

    public:
        explicit SchedulerImpl(PiperContext& context) noexcept
            : Scheduler(context), mThreadPool(context.getAllocator()), mContext(context.getAllocator()) {
            const size_t workers = std::thread::hardware_concurrency();
            for(Index i = 0; i < workers; ++i)
                mThreadPool.push_back(std::thread{ worker, std::reference_wrapper<SharedContext>(mContext) });
        }
        SharedPtr<FutureImpl> newFutureImpl(const size_t size, const bool) override {
            return makeSharedObject<FutureStorage>(context(), size);
        }
        void spawnImpl(Optional<Closure<>> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                       const SharedPtr<FutureImpl>& res) override {
            auto result = eastl::dynamic_shared_pointer_cast<FutureStorage>(res);
            result->setRemain(1);
            STLAllocator allocator = context().getAllocator();
            auto delay =
                makeUniquePtr<DelayedTask>(allocator, allocator, Task{ decltype(Task::work){ std::move(func) }, result });
            emitTask(delay.get(), dependencies);
            delay.release();
        }
        void parallelForImpl(const uint32_t n, Closure<uint32_t> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                             const SharedPtr<FutureImpl>& res) override {
            auto result = eastl::dynamic_shared_pointer_cast<FutureStorage>(res);
            auto chunkSize = std::max(1U, n / static_cast<uint32_t>(mThreadPool.size() * 3));
            auto blockCount = (n + chunkSize - 1) / chunkSize;
            result->setRemain(blockCount);
            auto closure = makeSharedObject<Closure<uint32_t>>(context(), context().getAllocator(),
                                                               [call = std::move(func), chunkSize, n](const uint32_t idx) {
                                                                   auto beg = idx * chunkSize;
                                                                   const auto end = std::min(beg + chunkSize, n);
                                                                   while(beg < end) {
                                                                       call(beg);
                                                                       ++beg;
                                                                   }
                                                               });
            STLAllocator allocator = context().getAllocator();
            auto delay = makeUniquePtr<DelayedTask>(
                allocator, allocator, Task{ decltype(Task::work){ makePair(std::move(closure), blockCount) }, result });
            emitTask(delay.get(), dependencies);
            delay.release();
        }
        bool supportNotify() const noexcept override {
            return true;
        }
        void notify(FutureImpl* event) override {
            std::unique_lock<std::mutex> guard{ mContext.NEFMutex };
            auto&& cont = mContext.notifyExternalFuture;
            auto iter = cont.find(event);
            if(iter == cont.cend())
                return;
            auto future = std::move(iter->second);
            cont.erase(iter);
            guard.unlock();

            for(auto&& successor : future.successor)
                decreaseDepCount(mContext, successor);
        }
        ~SchedulerImpl() noexcept override {
            mContext.run = false;
            mContext.cv.notify_all();
            for(auto&& worker : mThreadPool)
                worker.join();
        }
    };

    class ModuleImpl final : public Module {
    public:
        ModuleImpl(PiperContext& context, const char*) : Module(context) {}
        Future<SharedPtr<Object>> newInstance(const StringView& classID, const SharedPtr<Config>& config,
                                              const Future<void>& module) override {
            if(classID == "Scheduler") {
                return context().getScheduler().value(
                    eastl::static_shared_pointer_cast<Object>(makeSharedObject<SchedulerImpl>(context())));
            }
            context().getErrorHandler().unresolvedClassID(classID, PIPER_SOURCE_LOCATION());
        }
    };
}  // namespace Piper

PIPER_INIT_MODULE_IMPL(Piper::ModuleImpl)
