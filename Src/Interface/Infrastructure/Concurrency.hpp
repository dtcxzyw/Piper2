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
#include "../../STL/DynamicArray.hpp"
#include "../../STL/Optional.hpp"
#include "../../STL/String.hpp"
#include "../../STL/UniquePtr.hpp"
#include "ErrorHandler.hpp"

#include <future>

namespace Piper {
    // TODO:process report
    class FutureImpl : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(FutureImpl, Object)
        virtual ~FutureImpl() = default;
        [[nodiscard]] virtual bool fastReady() const noexcept = 0;
        [[nodiscard]] virtual bool ready() const noexcept = 0;
        virtual void wait() const = 0;
        [[nodiscard]] virtual const void* storage() const = 0;
        [[nodiscard]] virtual bool supportNotify() const noexcept {
            return false;
        }
    };

    namespace detail {
        template <typename T>
        struct RemovePointer {
            static T* getPointer(T* val) {
                return val;
            }
            static const T* getPointer(const T* val) {
                return val;
            }
        };
        template <typename T, typename D>
        struct RemovePointer<UniquePtr<T, D>> {
            static T* getPointer(UniquePtr<T, D>* val) {
                return val->get();
            }
            static const T* getPointer(const UniquePtr<T, D>* val) {
                return val->get();
            }
        };
        template <typename T>
        struct RemovePointer<SharedPtr<T>> {
            static T* getPointer(SharedPtr<T>* val) {
                return val->get();
            }
            static const T* getPointer(const SharedPtr<T>* val) {
                return val->get();
            }
        };
    }  // namespace detail

    template <typename T>
    class Future final {
    private:
        SharedPtr<FutureImpl> mImpl;
        mutable bool mReady;

    public:
        explicit Future(SharedPtr<FutureImpl> impl) noexcept : mImpl(std::move(impl)), mReady(mImpl->ready()) {}

        const SharedPtr<FutureImpl>& raw() const {
            return mImpl;
        }
        bool ready() const noexcept {
            if(mReady)
                return true;
            const auto res = mImpl->ready();
            if(res)
                mReady = true;
            return res;
        }
        void wait() const {
            if(!ready())
                return mImpl->wait();
        }

        decltype(auto) operator*() const {
            return *operator->();
        }

        template <typename U = T>
        decltype(auto) operator*() {
            return *operator->();
        }

        auto operator->() const {
            if(!ready())
                throw;
            return detail::RemovePointer<T>::getPointer(static_cast<const T*>(mImpl->storage()));
        }

        auto operator->() {
            if(!ready())
                throw;
            return detail::RemovePointer<T>::getPointer(static_cast<T*>(const_cast<void*>(mImpl->storage())));
        }

        T& get() {
            return *static_cast<T*>(const_cast<void*>(mImpl->storage()));
        }

        const T& get() const {
            return *static_cast<const T*>(mImpl->storage());
        }

        ~Future() noexcept {
            // TODO:formal check
            if(mImpl && mImpl.unique() && !ready()) {
                mImpl->context().getErrorHandler().raiseException("Not handled future", PIPER_SOURCE_LOCATION());
            }
        }
    };

    template <>
    class Future<void> final {
    private:
        mutable SharedPtr<FutureImpl> mImpl;

    public:
        explicit Future(SharedPtr<FutureImpl> impl) noexcept : mImpl(std::move(impl)) {}

        const SharedPtr<FutureImpl>& raw() const {
            return mImpl;
        }
        bool ready() const noexcept {
            if(mImpl) {
                if(mImpl->ready()) {
                    mImpl.reset();
                    return true;
                }
                return false;
            }
            return true;
        }
        void wait() const {
            if(!ready())
                mImpl->wait();
        }
        ~Future() noexcept {
            // TODO:formal check
            if(mImpl && mImpl.unique() && !ready()) {
                throw;
            }
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

    // TODO:remove Closure
    template <typename... Args>
    class Closure final {
    private:
        PiperContext& mContext;
        UniquePtr<ClosureStorage<Args...>> mFunc;
        // mutable std::atomic_size_t mCount;

        template <typename Callable>
        Owner<ClosureStorage<Args...>*> alloc(STLAllocator allocator, Callable&& func) {
            auto ptr = static_cast<ClosureStorageImpl<Callable, Args...>*>(
                allocator.allocate(sizeof(ClosureStorageImpl<Callable, Args...>)));
            return new(ptr) ClosureStorageImpl<Callable, Args...>(std::move(func));
        }

    public:
        Closure() = delete;
        template <typename Callable>
        Closure(PiperContext& context, STLAllocator allocator, Callable&& func)
            : mContext(context), mFunc(alloc<Callable>(allocator, std::move(func)),
                                       DefaultDeleter<ClosureStorage<Args...>>{ allocator }) /*,mCount(0)*/ {}
        Closure(const Closure& rhs)
            : mContext(rhs.mContext),
              mFunc(std::move(const_cast<UniquePtr<ClosureStorage<Args...>>&>(rhs.mFunc))) /*, mCount(0)*/ {}
        void operator()(Args... args) const {
            if(!mFunc)
                throw;
            mFunc->apply(args...);
            //++mCount;
        }
        /*
        ~Closure()  {
            if(mCount > 1) {
                auto& logger = mContext.getLogger();
                if(logger.allow(LogLevel::Debug))
                    logger.record(LogLevel::Debug,
                                  "Execute Count = " + toString(mContext.getAllocator(), static_cast<size_t>(mCount)),
                                  PIPER_SOURCE_LOCATION());
            }
        }
        */
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

        template <typename... Args>
        struct NoArgument final {
            static constexpr bool value = false;
        };

        template <>
        struct NoArgument<> final {
            static constexpr bool value = true;
        };

    }  // namespace detail

    // TODO:support pipe
    // TODO:support future<instance>'s member function spawn before construction
    // TODO:support future of future(return future in spawn function)
    // TODO:asynchronous destruction
    // TODO:allow spawn in worker thread?
    class Scheduler : public Object {
    public:
        virtual void spawnImpl(Optional<Closure<>> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                               const SharedPtr<FutureImpl>& res) = 0;
        virtual void parallelForImpl(uint32_t n, Closure<uint32_t> func, const Span<const SharedPtr<FutureImpl>>& dependencies,
                                     const SharedPtr<FutureImpl>& res) = 0;
        virtual SharedPtr<FutureImpl> newFutureImpl(size_t size, bool ready) = 0;

    private:
        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<std::is_void_v<ReturnType>, Future<void>> spawnDispatch(Callable&& callable, Args&&... args) {
            // TODO:std::move dependencies
            const auto dependencies = std::initializer_list<SharedPtr<FutureImpl>>{ args.raw()... };
            const auto depSpan = Span<const SharedPtr<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);

            // zero argument optimization
            if constexpr(detail::NoArgument<Args...>::value) {
                spawnImpl(Closure<>{ context(), context().getAllocator(), std::forward<Callable>(callable) }, {}, result);
            } else {
                spawnImpl(
                    Closure<>{ context(), context().getAllocator(),
                               [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...)] {
                                   detail::moveApply(std::move(call),
                                                     std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple)));
                               } },
                    depSpan, result);
            }
            return Future<ReturnType>(result);
        }

        template <typename... T>
        struct IsFuture {
            static constexpr bool value = false;
        };

        template <typename... T>
        struct IsFuture<Future<T>...> {
            static constexpr bool value = true;
        };

        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<!std::is_void_v<ReturnType> && !IsFuture<ReturnType>::value, Future<ReturnType>>
        spawnDispatch(Callable&& callable, Args&&... args) {
            // TODO:std::move dependencies
            const auto dependencies = std::initializer_list<SharedPtr<FutureImpl>>{ args.raw()... };
            const auto depSpan = Span<const SharedPtr<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(sizeof(ReturnType), false);

            spawnImpl(Closure<>{ context(), context().getAllocator(),
                                 [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...),
                                  ptr = result->storage()] {
                                     new(static_cast<ReturnType*>(const_cast<void*>(ptr))) ReturnType(detail::moveApply(
                                         std::move(call), std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple))));
                                 } },
                      depSpan, result);

            return Future<ReturnType>{ std::move(result) };
        }

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

        // ReSharper disable once CppMemberFunctionMayBeStatic
        [[nodiscard]] Future<void> ready() const noexcept {
            return Future<void>(nullptr);
        }

        template <typename Callable, typename... Args, typename = std::enable_if_t<IsFuture<std::decay_t<Args>...>::value>>
        auto spawn(Callable&& callable, Args&&... args) {
            using ReturnType = std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>;
            return spawnDispatch<ReturnType>(std::forward<Callable>(callable), std::forward<Args>(args)...);
        }

        template <typename T>
        auto wrap(DynamicArray<Future<T>> futures) -> std::enable_if_t<!std::is_void_v<T>, Future<DynamicArray<T>>> {
            auto result = newFutureImpl(sizeof(DynamicArray<T>), false);
            DynamicArray<const SharedPtr<FutureImpl>> dep{ context().getAllocator() };
            dep.reserve(futures.size());
            for(auto&& future : futures)
                dep.push_back(future.raw());
            spawnImpl(Closure<>{ context(), context().getAllocator(),
                                 [fs = std::move(futures), ptr = result->storage(), allocator = &context().getAllocator()] {
                                     auto vec = static_cast<DynamicArray<T>*>(const_cast<void*>(ptr));
                                     new(vec) DynamicArray<T>(*allocator);
                                     vec->reserve(fs.size());
                                     for(auto&& future : const_cast<DynamicArray<Future<T>>&>(fs))
                                         // TODO:ownership
                                         vec->emplace_back(std::move(future.get()));
                                 } },
                      Span<const SharedPtr<FutureImpl>>{ dep.data(), dep.size() }, result);
            return Future<DynamicArray<T>>{ std::move(result) };
        }

        // TODO:reduce empty node
        template <typename T>
        auto wrap(const DynamicArray<Future<T>>& futures) -> std::enable_if_t<std::is_void_v<T>, Future<void>> {
            auto result = newFutureImpl(0, false);
            DynamicArray<const SharedPtr<FutureImpl>> dep{ context().getAllocator() };
            dep.reserve(futures.size());
            for(auto&& future : futures)
                dep.push_back(future.raw());
            spawnImpl(Optional<Closure<>>(eastl::nullopt), Span<const SharedPtr<FutureImpl>>{ dep.data(), dep.size() }, result);
            return Future<void>{ std::move(result) };
        }

        [[nodiscard]] virtual bool supportNotify() const noexcept {
            return false;
        }
        virtual void notify(FutureImpl* event) {}

        template <typename Callable, typename... Args, typename = std::enable_if_t<IsFuture<std::decay_t<Args>...>::value>>
        Future<void> parallelFor(const uint32_t n, Callable&& callable, Args&&... args) {
            const auto dependencies = std::initializer_list<SharedPtr<FutureImpl>>{ args.raw()... };
            const auto depSpan = Span<const SharedPtr<FutureImpl>>(dependencies.begin(), dependencies.end());
            auto result = newFutureImpl(0, false);

            // TODO:copy?
            parallelForImpl(
                n,
                Closure<uint32_t>{
                    context(), context().getAllocator(),
                    [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...),
                     ptr = result->storage()](uint32_t idx) { std::apply(call, std::tuple_cat(std::make_tuple(idx), tuple)); } },
                depSpan, result);

            return Future<void>{ std::move(result) };
        }
        // TODO: reduce(ordered/unordered? + associative laws)
    };

}  // namespace Piper
