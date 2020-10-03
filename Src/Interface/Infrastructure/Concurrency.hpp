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

#pragma once
#include "../../STL/Function.hpp"
#include "../ContextResource.hpp"

namespace Piper {
    class DataSpan;
    template <typename T>
    class DataView;

    class FutureImpl : public ContextResource {
    public:
        PIPER_INTERFACE_CONSTRUCT(FutureImpl, ContextResource)
        virtual ~FutureImpl() = default;
        virtual bool ready() const noexcept = 0;
        virtual void wait() const = 0;
        virtual void* storage() const = 0;
    };

    template <typename T>
    class Future final {
    private:
        SharedObject<FutureImpl> mImpl;

    public:
        explicit Future(const SharedObject<FutureImpl>& impl) noexcept : mImpl(impl) {}
        SharedObject<FutureImpl> raw() const {
            return mImpl;
        }
        bool ready() const noexcept {
            return mImpl->ready();
        }
        void wait() const {
            return mImpl->wait();
        }
        T& get() {
            mImpl->wait();
            return *reinterpret_cast<T*>(mImpl->storage());
        }
        const T& get() const {
            mImpl->wait();
            return *reinterpret_cast<T*>(mImpl->storage());
        }
    };

    template <>
    class Future<void> final {
    private:
        SharedObject<FutureImpl> mImpl;

    public:
        explicit Future(const SharedObject<FutureImpl>& impl) noexcept : mImpl(impl) {}
        SharedObject<FutureImpl> raw() const {
            return mImpl;
        }
        bool ready() const noexcept {
            return mImpl->ready();
        }
        void wait() const {
            return mImpl->wait();
        }
    };

    /*
        class FutureGroupImpl : public ContextResource {
        public:
            PIPER_INTERFACE_CONSTRUCT(FutureGroupImpl, ContextResource)
            virtual ~FutureGroupImpl() = 0;
            virtual const DataSpan& get(const size_t beg, const size_t end) const = 0;
            virtual size_t size() const noexcept = 0;
            DataSpan& get(const size_t beg, const size_t end) final {
                return const_cast<DataSpan&>(const_cast<const FutureGroupImpl*>(this)->get(beg, end));
            }
        };

        template <typename T>
        class FutureGroup final : private Unmovable {
        private:
            SharedObject<FutureGroupImpl> mImpl;

        public:
            FutureGroup() = delete;
            explicit FutureGroup(SharedObject<FutureGroup>&& impl) noexcept : mImpl(impl) {}
            SharedObject<FutureGroupImpl> raw() const;
            DataView<T> get(const size_t beg, const size_t end) {
                return mImpl->get(beg, end);
            }
            DataView<T> get(const size_t beg, const size_t end) const {
                return mImpl->get(beg, end);
            }
        };
        */

    // TODO:this_thread_scheduler::yield
    class Scheduler : public ContextResource {
    protected:
        virtual void spawnImpl(const Function<void>& func, const Span<const SharedObject<FutureImpl>>& dependencies,
                               const SharedObject<FutureImpl>& res) = 0;
        virtual SharedObject<FutureImpl> newFutureImpl(const size_t size, const bool ready) = 0;

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<std::is_void_v<ReturnType>>*, Callable&& callable, const Future<Args>&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);
            auto tuple = std::make_tuple(args...);
            spawnImpl(Function<void>([call = std::forward<Callable>(callable), tuple] { std::apply(call, tuple); }), depSpan,
                      result);

            return Future<ReturnType>(result);
        }

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<!std::is_void_v<ReturnType>>*, Callable&& callable, const Future<Args>&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(sizeof(ReturnType), false);
            auto tuple = std::make_tuple(args...);

            spawnImpl(Function<void>([call = std::forward<Callable>(callable), tuple, ptr = result->storage()] {
                          new(reinterpret_cast<ReturnType*>(ptr)) ReturnType(std::move(std::apply(call, tuple)));
                      }),
                      depSpan, result);

            return Future<ReturnType>(result);
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(Scheduler, ContextResource)
        virtual ~Scheduler() = default;

        // TODO:move
        template <typename T>
        auto value(const T& value) {
            auto res = newFutureImpl(sizeof(T), true);
            new(static_cast<T*>(res->storage())) T(value);
            return Future<T>{ res };
        }

        // TODO:zero allocation
        Future<void> ready() {
            auto res = newFutureImpl(0, true);
            return Future<void>(res);
        }

        template <typename Callable, typename... Args>
        auto spawn(Callable&& callable, const Future<Args>&... args) {
            using ReturnType = std::invoke_result_t<Callable, decltype(args)...>;
            return spawnDispatch<ReturnType, Callable, Args...>(nullptr, std::forward<Callable>(callable), args...);
        }

        virtual void yield() noexcept = 0;  // TODO:Coroutine
        virtual void waitAll() noexcept = 0;
        // virtual size_t getCurrentTaskID() const noexcept = 0;

        // parallel_for
        // reduce
    };  // namespace Piper

}  // namespace Piper
