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
    struct PlaceHolder final {};

    namespace Detail {

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

        // Temporary Auxiliary Variable
        template <typename T, typename S>
        class FutureCallProxy final {
        private:
            const SharedPtr<FutureImpl>& mFuture;
            T* mPtr;
            S mCall;

        public:
            FutureCallProxy(const SharedPtr<FutureImpl>& future, T* ptr, S call) : mFuture(future), mPtr(ptr), mCall(call) {}

            template <typename... Args>
            auto operator()(Args&&... args);
        };

        template <typename T, typename S>
        class FutureAccessProxy final {
        private:
            const SharedPtr<FutureImpl>& mFuture;
            const T* mPtr;
            S mAccess;

        public:
            FutureAccessProxy(const SharedPtr<FutureImpl>& future, const T* ptr, S access)
                : mFuture(future), mPtr(ptr), mAccess(access) {}

            auto operator()();
        };

        template <typename T, typename S>
        using FutureProxyDispatch =
            std::conditional_t<std::is_member_function_pointer_v<S>, FutureCallProxy<T, S>, FutureAccessProxy<T, S>>;
    }  // namespace Detail

    // TODO:inplace operation?
    template <typename T>
    class Future final {
    private:
        SharedPtr<FutureImpl> mImpl;
        mutable bool mReady;

    public:
        using PointerT = decltype(Detail::RemovePointer<T>::getPointer(std::declval<T*>()));
        explicit Future(SharedPtr<FutureImpl> impl) noexcept : mImpl(std::move(impl)), mReady(mImpl->ready()) {}

        PiperContext& context() const {
            return mImpl->context();
        }

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
            if(!ready()) {
                mImpl->wait();
                mReady = true;
            }
        }

        T& get() {
            return *static_cast<T*>(const_cast<void*>(mImpl->storage()));
        }

        const T& get() const {
            return *static_cast<const T*>(mImpl->storage());
        }

        T& getSync() {
            wait();
            return *static_cast<T*>(const_cast<void*>(mImpl->storage()));
        }

        const T& getSync() const {
            wait();
            return *static_cast<const T*>(mImpl->storage());
        }

        template <typename S>
        auto operator->*(S call) {
            return Detail::FutureProxyDispatch<T, S>(mImpl, static_cast<T*>(const_cast<void*>(mImpl->storage())), call);
        }
        template <typename S>
        auto operator->*(S call) const {
            return Detail::FutureProxyDispatch<const T, S>(mImpl, static_cast<const T*>(mImpl->storage()), call);
        }
    };

#define PIPER_FUTURE_CALL(inst, call) ((inst)->*(&(std::remove_pointer_t<std::decay_t<typename decltype(inst)::PointerT>>::call)))
#define PIPER_FUTURE_ACCESS(inst, data) \
    ((inst)->*(&std::remove_pointer_t<std::decay_t<typename decltype(inst)::PointerT>>::data))()

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
                    // TODO: ABA?
                    // mImpl.reset();
                    return true;
                }
                return false;
            }
            return true;
        }
        void wait() const {
            if(!ready()) {
                mImpl->wait();
                // TODO: ABA?
                // mImpl.reset();
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
            return new(ptr) ClosureStorageImpl<Callable, Args...>(std::forward<Callable>(func));
        }

    public:
        Closure() = delete;
        Closure(Closure&&) = default;
        template <typename Callable>
        Closure(PiperContext& context, Callable&& func)
            : mContext(context),
              mFunc(alloc<Callable>(STLAllocator{ context.getAllocator() }, std::forward<Callable>(func)),
                    DefaultDeleter<ClosureStorage<Args...>>{ STLAllocator{ context.getAllocator() } }) /*,mCount(0)*/ {}
        Closure(const Closure& rhs)
            : mContext(rhs.mContext),
              mFunc(std::move(const_cast<UniquePtr<ClosureStorage<Args...>>&>(rhs.mFunc))) /*, mCount(0)*/ {}
        void operator()(Args... args) const {
            if(!mFunc)
                mContext.getErrorHandler().raiseException("Invalid target.", PIPER_SOURCE_LOCATION());
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

    namespace Detail {
        template <typename... Args>
        struct NoArgument final {
            static constexpr bool value = false;
        };

        template <>
        struct NoArgument<> final {
            static constexpr bool value = true;
        };

        template <typename T>
        struct RemoveFuture final {
            using Type = T;
        };

        template <typename T>
        struct RemoveFuture<Future<T>> final {
            using Type = std::enable_if_t<!std::is_void_v<T>, T>;
        };
        template <>
        struct RemoveFuture<Future<void>> final {
            using Type = PlaceHolder;
        };

        template <typename T>
        struct IsFuture {
            static constexpr bool value = false;
        };

        template <typename T>
        struct IsFuture<Future<T>> {
            static constexpr bool value = true;
        };

        template <typename T, typename = std::enable_if_t<!IsFuture<T>::value>>
        decltype(auto) removeFuture(T&& arg) {
            return (std::forward<T>(arg));
        }

        template <typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
        decltype(auto) removeFuture(const Future<T>& arg) {
            return (arg.get());
        }
        template <typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
        decltype(auto) removeFuture(Future<T>&& arg) {
            return std::move(arg.get());
        }

        constexpr auto removeFuture(const Future<void>&) {
            return PlaceHolder{};
        }

        template <class Callable, class Tuple, size_t... Indices>
        constexpr decltype(auto) moveApplyImpl(Callable&& func, Tuple&& args, std::index_sequence<Indices...>) {
            return std::invoke(std::forward<Callable>(func),
                               removeFuture(std::move(std::get<Indices>(std::forward<Tuple>(args))))...);
        }

        template <class Callable, class Tuple>
        constexpr decltype(auto) moveApply(Callable&& func, Tuple&& args) {
            return moveApplyImpl(std::forward<Callable>(func), std::forward<Tuple>(args),
                                 std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
        }

        template <class Callable, class Tuple, size_t... Indices>
        constexpr decltype(auto) copyApplyImpl(Callable&& func, Tuple&& args, std::index_sequence<Indices...>) {
            return std::invoke(std::forward<Callable>(func), removeFuture(std::get<Indices>(std::forward<Tuple>(args)))...);
        }

        template <class Callable, class Tuple>
        constexpr decltype(auto) copyApply(Callable&& func, Tuple&& args) {
            return copyApplyImpl(std::forward<Callable>(func), std::forward<Tuple>(args),
                                 std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
        }

        template <typename T>
        SharedPtr<FutureImpl> raw(const Future<T>& arg) {
            return std::move(arg.raw());
        }
        template <typename T, typename = std::enable_if_t<!IsFuture<std::decay_t<T>>::value>>
        SharedPtr<FutureImpl> raw(T&&) {
            return nullptr;
        }

        template <typename T>
        using ReturnTypeCheck =
            std::enable_if_t<!(std::is_pointer_v<T> || std::is_lvalue_reference_v<T> || std::is_rvalue_reference_v<T>), T>;
    }  // namespace Detail

    // TODO:support generator
    // TODO:asynchronous destruction
    // TODO:better argument passing
    class Scheduler : public Object {
    public:
        virtual void spawnImpl(Optional<Closure<>> func, const Span<SharedPtr<FutureImpl>>& dependencies,
                               const SharedPtr<FutureImpl>& res) = 0;
        virtual void parallelForImpl(uint32_t n, Closure<uint32_t> func, const Span<SharedPtr<FutureImpl>>& dependencies,
                                     const SharedPtr<FutureImpl>& res) = 0;
        virtual SharedPtr<FutureImpl> newFutureImpl(size_t size, Closure<void*> deleter, bool ready) = 0;

    private:
        // ReturnType=void
        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<std::is_void_v<ReturnType>, Future<void>> spawnDispatch(Callable&& callable, Args&&... args) {
            auto result = newFutureImpl(0, Closure<void*>{ context(), [](void*) {} }, false);

            // zero argument optimization
            if constexpr(Detail::NoArgument<Args...>::value) {
                spawnImpl(Closure<>{ context(), std::forward<Callable>(callable) }, {}, result);
            } else {
                SharedPtr<FutureImpl> dependencies[] = { nullptr, std::move(Detail::raw(std::forward<Args>(args)))... };
                const auto depSpan = Span<SharedPtr<FutureImpl>>(std::begin(dependencies) + 1, std::end(dependencies));
                spawnImpl(
                    Closure<>{ context(),
                               [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...)] {
                                   Detail::moveApply(std::move(call),
                                                     std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple)));
                               } },
                    depSpan, result);
            }
            return Future<ReturnType>(result);
        }

        // ReturnType=T
        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<!std::is_void_v<ReturnType> && !Detail::IsFuture<ReturnType>::value, Future<ReturnType>>
        spawnDispatch(Callable&& callable, Args&&... args) {
            SharedPtr<FutureImpl> dependencies[] = { nullptr, std::move(Detail::raw(args))... };
            const auto depSpan = Span<SharedPtr<FutureImpl>>(std::begin(dependencies) + 1, std::end(dependencies));
            auto result = newFutureImpl(
                sizeof(ReturnType),
                Closure<void*>{ context(), [](void* ptr) { std::destroy_at(static_cast<ReturnType*>(ptr)); } }, false);

            spawnImpl(Closure<>{ context(),
                                 [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...),
                                  ptr = result->storage()] {
                                     new(static_cast<ReturnType*>(const_cast<void*>(ptr))) ReturnType(Detail::moveApply(
                                         std::move(call), std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple))));
                                 } },
                      depSpan, result);

            return Future<ReturnType>{ std::move(result) };
        }

        // ReturnType=Future<void>
        template <typename ReturnType, typename Callable, typename... Args>
        std::enable_if_t<std::is_same_v<ReturnType, Future<void>>, Future<void>> spawnDispatch(Callable&& callable,
                                                                                               Args&&... args) {
            auto result = newFutureImpl(0, Closure<void*>{ context(), [](void*) {} }, false);

            SharedPtr<FutureImpl> dependencies[] = { nullptr, std::move(Detail::raw(args))... };
            const auto depSpan = Span<SharedPtr<FutureImpl>>(std::begin(dependencies) + 1, std::end(dependencies));
            spawnImpl(Closure<>{ context(),
                                 [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...)] {
                                     Detail::moveApply(std::move(call),
                                                       std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple)))
                                         .wait();
                                     // TODO:yield time slice
                                 } },
                      depSpan, result);
            return Future<void>{ std::move(result) };
        }
        // ReturnType=Future<T>
        template <typename ReturnType, typename RealReturnType = typename Detail::RemoveFuture<ReturnType>::Type,
                  typename Callable, typename... Args>
        std::enable_if_t<Detail::IsFuture<ReturnType>::value && !std::is_same_v<ReturnType, Future<void>>, Future<RealReturnType>>
        spawnDispatch(Callable&& callable, Args&&... args) {
            SharedPtr<FutureImpl> dependencies[] = { nullptr, std::move(Detail::raw(args))... };
            const auto depSpan = Span<SharedPtr<FutureImpl>>(std::begin(dependencies) + 1, std::end(dependencies));

            static_assert(!Detail::IsFuture<RealReturnType>::value, "Future of Future is not supported.");
            auto result = newFutureImpl(
                sizeof(RealReturnType),
                Closure<void*>{ context(), [](void* ptr) { std::destroy_at(static_cast<RealReturnType*>(ptr)); } }, false);

            spawnImpl(Closure<>{ context(),
                                 [call = std::forward<Callable>(callable), tuple = std::make_tuple(std::forward<Args>(args)...),
                                  ptr = result->storage()] {
                                     auto future = std::move(Detail::moveApply(
                                         std::move(call), std::move(const_cast<std::remove_const_t<decltype(tuple)>&>(tuple))));
                                     // TODO:yield time slice
                                     future.wait();
                                     // TODO:ownership?
                                     new(static_cast<RealReturnType*>(const_cast<void*>(ptr)))
                                         RealReturnType(std::move(future.get()));
                                 } },
                      depSpan, result);

            return Future<RealReturnType>{ std::move(result) };
        }

    public:
        PIPER_INTERFACE_CONSTRUCT(Scheduler, Object)
        virtual ~Scheduler() = default;

        template <typename T>
        auto value(T&& value) {
            using StorageType = std::decay_t<T>;
            auto res = newFutureImpl(
                sizeof(StorageType),
                Closure<void*>{ context(), [](void* ptr) { std::destroy_at(static_cast<StorageType*>(ptr)); } }, true);
            new(static_cast<StorageType*>(const_cast<void*>(res->storage()))) StorageType(std::forward<T>(value));
            return Future<StorageType>{ std::move(res) };
        }

        // ReSharper disable once CppMemberFunctionMayBeStatic
        [[nodiscard]] Future<void> ready() const noexcept {
            return Future<void>{ nullptr };
        }

        template <typename Callable, typename... Args>
        auto spawn(Callable&& callable, Args&&... args) {
            using ReturnType = Detail::ReturnTypeCheck<
                std::invoke_result_t<std::decay_t<Callable>, typename Detail::RemoveFuture<std::decay_t<Args>>::Type...>>;
            return spawnDispatch<ReturnType>(std::forward<Callable>(callable), std::forward<Args>(args)...);
        }

        template <typename T>
        auto wrap(DynamicArray<Future<T>> futures) -> std::enable_if_t<!std::is_void_v<T>, Future<DynamicArray<T>>> {
            if(futures.empty())
                return value<DynamicArray<T>>({});
            auto result = newFutureImpl(
                sizeof(DynamicArray<T>),
                Closure<void*>{ context(), [](void* ptr) { std::destroy_at(static_cast<DynamicArray<T>*>(ptr)); } }, false);
            DynamicArray<SharedPtr<FutureImpl>> dep{ context().getAllocator() };
            dep.reserve(futures.size());
            for(auto&& future : futures)
                dep.push_back(std::move(future.raw()));
            spawnImpl(Closure<>{ context(),
                                 [fs = std::move(futures), ptr = result->storage(), ctx = &context()] {
                                     auto vec = static_cast<DynamicArray<T>*>(const_cast<void*>(ptr));
                                     new(vec) DynamicArray<T>(ctx->getAllocator());
                                     vec->reserve(fs.size());
                                     for(auto&& future : const_cast<DynamicArray<Future<T>>&>(fs))
                                         // TODO:ownership
                                         vec->emplace_back(std::move(future.get()));
                                 } },
                      Span<SharedPtr<FutureImpl>>{ dep.data(), dep.size() }, result);
            return Future<DynamicArray<T>>{ std::move(result) };
        }

        template <typename T>
        auto wrap(const DynamicArray<Future<T>>& futures) -> std::enable_if_t<std::is_void_v<T>, Future<void>> {
            if(futures.empty())
                return ready();
            auto result = newFutureImpl(0, Closure<void*>{ context(), [](void*) {} }, false);
            DynamicArray<SharedPtr<FutureImpl>> dep{ context().getAllocator() };
            dep.reserve(futures.size());
            for(auto&& future : futures)
                dep.push_back(future.raw());
            spawnImpl(Optional<Closure<>>(eastl::nullopt), Span<SharedPtr<FutureImpl>>{ dep.data(), dep.size() }, result);
            return Future<void>{ std::move(result) };
        }

        [[nodiscard]] virtual bool supportNotify() const noexcept {
            return false;
        }
        virtual void notify(FutureImpl* event) {}

        // TODO:support return value
        template <typename Callable, typename... Args,
                  typename ReturnType = std::invoke_result_t<std::decay_t<Callable>, uint32_t,
                                                             typename Detail::RemoveFuture<std::decay_t<Args>>::Type...>,
                  typename = std::enable_if_t<std::is_void_v<ReturnType>>>
        Future<void> parallelFor(const uint32_t n, Callable&& callable, Args&&... args) {
            if(n == 0)
                return ready();
            SharedPtr<FutureImpl> dependencies[] = { nullptr, std::move(Detail::raw(args))... };
            const auto depSpan = Span<SharedPtr<FutureImpl>>(std::begin(dependencies) + 1, std::end(dependencies));
            auto result = newFutureImpl(0, Closure<void*>{ context(), [](void*) {} }, false);

            parallelForImpl(n,
                            Closure<uint32_t>{ context(),
                                               [call = std::forward<Callable>(callable),
                                                tuple = std::make_tuple(std::forward<Args>(args)...)](uint32_t idx) {
                                                   Detail::copyApply(call, std::tuple_cat(std::make_tuple(idx), tuple));
                                               } },
                            depSpan, result);

            return Future<void>{ std::move(result) };
        }

        // TODO: reduce(ordered/unordered? + associative laws)

        template <typename T, typename... Args>
        Future<SharedPtr<Object>> constructObject(Args... args) {
            return spawn(
                [ctx = &context()](auto&&... inputs) {
                    return eastl::static_shared_pointer_cast<Object>(makeSharedObject<T>(*ctx, std::move(inputs)...));
                },
                std::move(args)...);
        }
    };

    template <typename T>
    class LValueReference final : public std::reference_wrapper<T> {
    private:
        SharedPtr<FutureImpl> mHolder;

    public:
        LValueReference(T& ref, SharedPtr<FutureImpl> owner) : std::reference_wrapper(ref), mHolder(std::move(owner)) {}
    };

    namespace Detail {
        template <typename T, typename = ReturnTypeCheck<T>>
        auto lvalueReference(T val, const SharedPtr<FutureImpl>&) {
            return val;
        }

        template <typename T>
        auto lvalueReference(T& val, SharedPtr<FutureImpl>&& holder) {
            return LValueReference<T>{ val, std::move(holder) };
        }

        template <typename T, typename S>
        template <typename... Args>
        auto FutureCallProxy<T, S>::operator()(Args&&... args) {
            // TODO:reduce copy
            return mFuture->context().getScheduler().spawn(
                [ref = mFuture, call = mCall, ptr = mPtr, tuple = std::make_tuple(std::forward<Args>(args)...)](PlaceHolder) {
#define PIPER_MOVE_CALL /* NOLINT(cppcoreguidelines-macro-usage)*/               \
    moveApply(call,                                                              \
              std::tuple_cat(std::make_tuple(RemovePointer<T>::getPointer(ptr)), \
                             const_cast<std::remove_const_t<decltype(tuple)>&>(tuple)))
                    using ReturnType = std::invoke_result_t<S, decltype(RemovePointer<T>::getPointer(std::declval<T*>())),
                                                            std::decay_t<Args>...>;
                    if constexpr(std::is_void_v<ReturnType>) {
                        PIPER_MOVE_CALL;
                    } else
                        return lvalueReference(PIPER_MOVE_CALL, std::move(const_cast<std::remove_const_t<decltype(ref)&>>(ref)));
                },
                Future<void>{ mFuture });
        }

        template <typename T, typename S>
        auto FutureAccessProxy<T, S>::operator()() {
            // TODO:reduce copy
            return mFuture->context().getScheduler().spawn(
                [ref = mFuture, access = mAccess, ptr = mPtr](PlaceHolder) { return RemovePointer<T>::getPointer(ptr)->*access; },
                Future<void>{ mFuture });
        }
    }  // namespace Detail

    template <typename T, typename U>
    Future<SharedPtr<T>> dynamicSharedPtrCast(const Future<SharedPtr<U>>& ptr) {
        return ptr.context().getScheduler().spawn([](SharedPtr<U> ptr) { return eastl::dynamic_shared_pointer_cast<T>(ptr); },
                                                  ptr);
    }

    // TODO:operator fusion?
#define PIPER_FUTURE_BINARY_OPERATOR(op) /* NOLINT(cppcoreguidelines-macro-usage)*/         \
                                                                                            \
    template <typename U, typename V>                                                       \
    auto operator##op(Future<U>&& u, Future<V>&& v) {                                       \
        return u.context().getScheduler().spawn([](U&& s, V&& t) { return s op t; }, u, v); \
    }
    PIPER_FUTURE_BINARY_OPERATOR(+)
    PIPER_FUTURE_BINARY_OPERATOR(-)
    PIPER_FUTURE_BINARY_OPERATOR(*)
    PIPER_FUTURE_BINARY_OPERATOR(/)
    PIPER_FUTURE_BINARY_OPERATOR(>>)
    PIPER_FUTURE_BINARY_OPERATOR(<<)
    PIPER_FUTURE_BINARY_OPERATOR(&)
    PIPER_FUTURE_BINARY_OPERATOR(|)
    PIPER_FUTURE_BINARY_OPERATOR(^)
#undef PIPER_FUTURE_BINARY_OPERATOR

#define PIPER_FUTURE_UNARY_OPERATOR(op) /* NOLINT(cppcoreguidelines-macro-usage)*/ \
                                                                                   \
    template <typename U, typename V>                                              \
    auto operator##op(Future<U>&& u) {                                             \
        return u.context().getScheduler().spawn([](U&& s) { return op s; }, u);    \
    }

    PIPER_FUTURE_UNARY_OPERATOR(!)
    PIPER_FUTURE_UNARY_OPERATOR(-)
#undef PIPER_FUTURE_UNARY_OPERATOR

}  // namespace Piper
