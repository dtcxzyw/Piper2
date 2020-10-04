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
        virtual ~FutureImpl() = default;
        virtual bool ready() const noexcept = 0;
        virtual void wait() const = 0;
        virtual const void* storage() const = 0;
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
        const T& get() const& {
            mImpl->wait();
            return *reinterpret_cast<const T*>(mImpl->storage());
        }
        const T& get() const&& {
            mImpl->wait();
            return *reinterpret_cast<const T*>(mImpl->storage());
        }
        T& get() & {
            mImpl->wait();
            return *reinterpret_cast<T*>(const_cast<void*>(mImpl->storage()));
        }
        T&& get() && {
            mImpl->wait();
            return std::move(*reinterpret_cast<T*>(const_cast<void*>(mImpl->storage())));
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
            return !mImpl || mImpl->ready();
        }
        void wait() const {
            if(mImpl)
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

    class Scheduler : public Object {
    protected:
        virtual void spawnImpl(Function<void>&& func, const Span<const SharedObject<FutureImpl>>& dependencies,
                               const SharedObject<FutureImpl>& res) = 0;
        virtual SharedObject<FutureImpl> newFutureImpl(const size_t size, const bool ready) = 0;

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<std::is_void_v<ReturnType>>*, Callable&& callable, Args&&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);
            spawnImpl(Function<void>(
                          [call = std::forward<Callable>(callable),
                           tuple = std::forward_as_tuple<Args...>(std::forward<Args>(args)...)] { std::apply(call, tuple); }),
                      depSpan, result);

            return Future<ReturnType>(result);
        }

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<!std::is_void_v<ReturnType>>*, Callable&& callable, Args&&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(sizeof(ReturnType), false);

            spawnImpl(
                Function<void>([call = std::forward<Callable>(callable),
                                tuple = std::forward_as_tuple<Args...>(std::forward<Args>(args)...), ptr = result->storage()] {
                    new(reinterpret_cast<ReturnType*>(const_cast<void*>(ptr))) ReturnType(std::apply(call, tuple));
                }),
                depSpan, result);

            return Future<ReturnType>(result);
        }

        template <typename... T>
        struct FutureChecker {
            static constexpr bool value = false;
        };

        template <typename... T>
        struct FutureChecker<Future<T>...> {
            static constexpr bool value = true;
        };

    public:
        PIPER_INTERFACE_CONSTRUCT(Scheduler, Object)
        virtual ~Scheduler() = default;

        template <typename T>
        auto value(T&& value) {
            using StorageType = std::decay_t<T>;
            auto res = newFutureImpl(sizeof(StorageType), true);
            new(static_cast<StorageType*>(const_cast<void*>(res->storage()))) StorageType(std::forward<T>(value));
            return Future<StorageType>{ res };
        }

        Future<void> ready() {
            return Future<void>(nullptr);
        }

        template <typename Callable, typename... Args, typename = std::enable_if_t<FutureChecker<std::decay_t<Args>...>::value>>
        auto spawn(Callable&& callable, Args&&... args) {
            using ReturnType = std::invoke_result_t<Callable, decltype(args)...>;
            return spawnDispatch<ReturnType, Callable, Args...>(nullptr, std::forward<Callable>(callable),
                                                                std::forward<Args>(args)...);
        }

        virtual void notify(FutureImpl* event) noexcept {}
        virtual void waitAll() noexcept = 0;

        // parallel_for
        // reduce
    };  // namespace Piper

}  // namespace Piper
