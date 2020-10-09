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
#include "../../PiperContext.hpp"
#include "../../STL/UniquePtr.hpp"
#include "../../STL/Variant.hpp"
#include "../../STL/Vector.hpp"

#include <future>

namespace Piper {

    // class DataSpan;
    // template <typename T>
    // class DataView;

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
        explicit Future(SharedObject<FutureImpl>&& impl) noexcept : mImpl(std::move(impl)) {}

        const SharedObject<FutureImpl>& raw() const {
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
        explicit Future(SharedObject<FutureImpl>&& impl) noexcept : mImpl(std::move(impl)) {}

        const SharedObject<FutureImpl>& raw() const {
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

    // void/index
    template <typename... Args>
    class ClosureStorage : public Unmovable {
    public:
        virtual ~ClosureStorage() = default;
        virtual void apply(Args... args) = 0;
    };
    template <typename Callable, typename... Args>
    class ClosureStorageImpl : public ClosureStorage<Args...> {
    private:
        Callable mFunc;

    public:
        explicit ClosureStorageImpl(Callable&& func) : mFunc(std::move(func)) {}
        void apply(Args... args) override {
            mFunc(args...);
        }
    };
    // TODO:execution count limit check
    template <typename... Args>
    class Closure final {
    private:
        UniquePtr<ClosureStorage<Args...>> mFunc;

        template <typename Callable>
        Owner<ClosureStorage<Args...>*> alloc(STLAllocator allocator, Callable&& func) {
            auto ptr = reinterpret_cast<ClosureStorageImpl<Callable, Args...>*>(
                allocator.allocate(sizeof(ClosureStorageImpl<Callable, Args...>)));
            return new(ptr) ClosureStorageImpl<Callable, Args...>(std::move(func));
        }

    public:
        Closure() = delete;
        template <typename Callable>
        Closure(STLAllocator allocator, Callable&& func)
            : mFunc(alloc<Callable>(allocator, std::move(func)), DefaultDeleter<ClosureStorage<Args...>>{ allocator }) {}
        Closure(const Closure& rhs) : mFunc(std::move(const_cast<UniquePtr<ClosureStorage<Args...>>&>(rhs.mFunc))) {}
        void operator()(Args... args) {
            if(mFunc)
                mFunc->apply(args...);
        }
    };

    namespace detail {
        template <class Callable, class Tuple, size_t... Indices>
        constexpr decltype(auto) moveApplyImpl(Callable&& func, Tuple&& args, std::index_sequence<Indices...>) {
            return std::invoke(std::forward<Callable>(func), std::move(std::get<Indices>(std::forward<Tuple>(args)))...);
        }

        template <class Callable, class Tuple>
        constexpr decltype(auto) moveApply(Callable&& func, Tuple&& args) {
            return moveApplyImpl(std::forward<Callable>(func), std::forward<Tuple>(args),
                                 std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
        }
    }  // namespace detail

    // TODO:support pipe
    // TODO:support future<instance>'s member function spawn before construction
    // TODO:support future of future(return future in spawn function)
    class Scheduler : public Object {
    public:
        virtual void spawnImpl(Variant<MonoState, Closure<>> func, const Span<const SharedObject<FutureImpl>>& dependencies,
                               const SharedObject<FutureImpl>& res) = 0;
        virtual void parallelForImpl(uint32_t n, Closure<uint32_t> func, const Span<const SharedObject<FutureImpl>>& dependencies,
                                     const SharedObject<FutureImpl>& res) = 0;
        virtual SharedObject<FutureImpl> newFutureImpl(const size_t size, const bool ready) = 0;

    private:
        template <typename... Args>
        struct NoArgument final {
            static constexpr bool value = false;
        };

        template <>
        struct NoArgument<> final {
            static constexpr bool value = true;
        };

        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<std::is_void_v<ReturnType>, Future<void>> spawnDispatch(Callable&& callable, Args&&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);

            if constexpr(NoArgument<Args...>::value) {
                spawnImpl(Closure{ context().getAllocator(), std::forward<Callable>(callable) }, {}, result);
            } else {
                spawnImpl(
                    Closure{ context().getAllocator(),
                             [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...)] {
                                 detail::moveApply(std::move(call),
                                                   std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple)));
                             } },
                    depSpan, result);
            }
            return Future<ReturnType>(result);
        }

        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<!std::is_void_v<ReturnType>, Future<ReturnType>> spawnDispatch(Callable&& callable, Args&&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(sizeof(ReturnType), false);

            spawnImpl(Closure{ context().getAllocator(),
                               [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...),
                                ptr = result->storage()] {
                                   new(reinterpret_cast<ReturnType*>(const_cast<void*>(ptr))) ReturnType(detail::moveApply(
                                       std::move(call), std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple))));
                               } },
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
            return Future<StorageType>{ std::move(res) };
        }

        Future<void> ready() {
            return Future<void>(nullptr);
        }

        template <typename Callable, typename... Args, typename = std::enable_if_t<FutureChecker<std::decay_t<Args>...>::value>>
        auto spawn(Callable&& callable, Args&&... args) {
            using ReturnType = std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>;
            return spawnDispatch<ReturnType>(std::forward<Callable>(callable), std::forward<Args>(args)...);
        }

        template <typename T>
        auto wrap(Vector<Future<T>> futures) -> std::enable_if_t<!std::is_void_v<T>, Future<Vector<T>>> {
            auto result = newFutureImpl(sizeof(Vector<T>), false);
            Vector<const SharedObject<FutureImpl>> dep{ context().getAllocator() };
            dep.reserve(futures.size());
            for(auto&& future : futures)
                dep.push_back(future.raw());
            spawnImpl(Closure{ context().getAllocator(),
                               [fs = std::move(futures), ptr = result->storage(), allocator = &context().getAllocator()] {
                                   auto vec = reinterpret_cast<Vector<T>*>(const_cast<void*>(ptr));
                                   new(vec) Vector<T>(*allocator);
                                   vec->reserve(fs.size());
                                   for(auto&& future : const_cast<Vector<Future<T>>&>(fs))
                                       // TODO:ownership
                                       vec->emplace_back(std::move(std::move(future).get()));
                               } },
                      Span<const SharedObject<FutureImpl>>{ dep.data(), dep.size() }, result);
            return Future<Vector<T>>{ result };
        }

        template <typename T>
        auto wrap(const Vector<Future<T>>& futures) -> std::enable_if_t<std::is_void_v<T>, Future<void>> {
            auto result = newFutureImpl(0, false);
            Vector<const SharedObject<FutureImpl>> dep;
            dep.reserve(futures.size());
            for(auto&& future : futures)
                dep.push_back(future.raw());
            spawnImpl(MonoState{}, Span<const SharedObject<FutureImpl>>{ dep.data(), dep.size() }, result);
            return Future<void>{ result };
        }

        virtual void notify(FutureImpl* event) noexcept {}
        virtual void waitAll() noexcept = 0;

        // TODO:hint for schedule strategy
        template <typename Callable, typename... Args, typename = std::enable_if_t<FutureChecker<std::decay_t<Args>...>::value>>
        Future<void> parallelFor(uint32_t n, Callable&& callable, Args&&... args) {
            auto dependencies = std::initializer_list<SharedObject<FutureImpl>>{ args.raw()... };
            auto depSpan = Span<const SharedObject<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);

            // TODO:copy?
            parallelForImpl(
                n,
                Closure<uint32_t>{
                    context().getAllocator(),
                    [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...),
                     ptr = result->storage()](uint32_t idx) { std::apply(call, std::tuple_cat(std::make_tuple(idx), tuple)); } },
                depSpan, result);

            return Future<void>(result);
        }
        // TODO: reduce
    };

}  // namespace Piper
