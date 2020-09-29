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
        virtual ~FutureImpl() = 0{}
        virtual bool ready() const noexcept = 0;
        virtual const void* get() const = 0;
        void* get() {
            return const_cast<void*>(const_cast<const FutureImpl*>(this)->get());
        }
    };

    template <typename T>
    class Future final : private Unmovable {
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
    class Future<void> final : private Unmovable {
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
        virtual SharedObject<FutureImpl> newFutureImpl(const size_t size, const void* value) = 0;

        template <typename T>
        struct IsFuture {
            static constexpr bool value = false;
        };

        template <typename T>
        struct IsFuture<Future<T>> {
            static constexpr bool value = true;
        };

        template <size_t x, class T>
        struct Insert {};

        template <size_t x, size_t... I>
        struct Insert<x, std::index_sequence<I...>> {
            using type = std::index_sequence<x, I...>;
        };

        template <size_t index, typename... Args>
        struct FutureSequence {};

        template <size_t index>
        struct FutureSequence<index> {
            using type = std::index_sequence<>;
        };

        template <size_t index, typename First, typename... Args>
        struct FutureSequence<index, Future<First>, Args...> {
            using type = typename Insert<index, typename FutureSequence<index + 1, Args...>::type>::type;
        };

        template <size_t index, typename First, typename... Args>
        struct FutureSequence<index, First, Args...> {
            using type = typename FutureSequence<index + 1, Args...>::type;
        };

        template <typename Tuple, size_t... I>
        std::initializer_list<SharedObject<FutureImpl>> extractTuple(Tuple&& tuple, std::index_sequence<I...>) {
            return { std::get<I>(std::forward<Tuple>(tuple))... };
        }
        template <typename... Args>
        auto extractFuture(Args&&... args) {
            using Index = typename FutureSequence<0, std::decay_t<Args>...>::type;
            return extractTuple(std::make_tuple(std::forward<Args>(args)...), Index{});
        }

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<std::is_void_v<ReturnType>>* ptr, Callable&& callable, Args&&... args) {
            throw;
            // copy
            auto dependencies = extractFuture(std::forward<Args>(args)...);
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, nullptr);
            // copy
            spawnImpl(Function<void, void*>(
                          [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...)](void*) {
                              std::apply(std::move(call), std::move(tuple));
                          }),
                      depSpan, result);

            return Future<ReturnType>(result);
        }

        template <typename ReturnType, typename Callable, typename... Args>
        auto spawnDispatch(std::enable_if_t<!std::is_void_v<ReturnType>>* ptr, Callable&& callable, Args&&... args) {
            throw;
            // copy
            auto dependencies = extractFuture(std::forward<Args>(args)...);
            auto result = newFutureImpl(sizeof(ReturnType), nullptr);

            spawnImpl(
                Function<void, void*>(
                    [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...)](void* res) {
                        *reinterpret_cast<ReturnType*>(res) = std::apply(std::forward<Callable>(call), std::move(tuple));
                    },
                    context().getAllocator()),
                dependencies, result);

            return Future<ReturnType>(result);
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(Scheduler, Object)
        virtual ~Scheduler() = 0{}

        template <typename T>
        auto value(T&& value) {
            return Future<T>(newFutureImpl(sizeof(T), &value));
        }

        template <typename Callable, typename... Args>
        auto spawn(Callable&& callable, Args&&... args) {
            using ReturnType = std::invoke_result_t<Callable, Args...>;
            return spawnDispatch<ReturnType, Callable, Args...>(nullptr, std::forward<Callable>(callable),
                                                                std::forward<Args>(args)...);
        }

        virtual void waitAll() noexcept = 0;

        // parallel_for
        // reduce
    };

}  // namespace Piper
