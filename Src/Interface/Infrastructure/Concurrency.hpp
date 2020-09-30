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

    class FutureImpl : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(FutureImpl, Object)
        virtual ~FutureImpl() = 0 {}
        virtual bool ready() const noexcept = 0;
        virtual const void* get() const = 0;
        void* get() {
            return const_cast<void*>(const_cast<const FutureImpl*>(this)->get());
        }
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
        T& get() {
            return *reinterpret_cast<T*>(mImpl->get());
        }
        const T& get() const {
            return *reinterpret_cast<T*>(mImpl->get());
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
        void get() const {
            mImpl->get();
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

    class Scheduler : public Object {
    protected:
        virtual void spawnImpl(const Function<void, void*>& func, const Span<const SharedObject<FutureImpl>>& dependencies,
                               const SharedObject<FutureImpl>& res) = 0;
        virtual SharedObject<FutureImpl> newFutureImpl(const size_t size, const bool ready) = 0;

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<std::is_void_v<ReturnType>>*, Callable&& callable, const Future<Args>&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);
            auto tuple = std::make_tuple(args...);
            spawnImpl(Function<void, void*>([call = std::forward<Callable>(callable), tuple](void*) { std::apply(call, tuple); }),
                      depSpan, result);

            return Future<ReturnType>(result);
        }

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<!std::is_void_v<ReturnType>>*, Callable&& callable, const Future<Args>&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(sizeof(ReturnType), false);
            auto tuple = std::make_tuple(args...);

            spawnImpl(Function<void, void*>([call = std::forward<Callable>(callable), tuple](void* res) {
                          new(reinterpret_cast<ReturnType*>(res)) ReturnType(std::move(std::apply(call, tuple)));
                      }),
                      depSpan, result);

            return Future<ReturnType>(result);
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(Scheduler, Object)
        virtual ~Scheduler() = 0 {}

        // TODO:move
        template <typename T>
        auto value(const T& value) {
            auto res = newFutureImpl(sizeof(T), true);
            new(static_cast<T*>(res->get())) T(value);
            return Future<T>{ res };
        }

        template <typename Callable, typename... Args>
        auto spawn(Callable&& callable, const Future<Args>&... args) {
            using ReturnType = std::invoke_result_t<Callable, decltype(args)...>;
            return spawnDispatch<ReturnType, Callable, Args...>(nullptr, std::forward<Callable>(callable), args...);
        }

        virtual void waitAll() noexcept = 0;

        // parallel_for
        // reduce
    };  // namespace Piper

}  // namespace Piper
