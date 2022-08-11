/*
   Copyright 2020-2021 Yingwei Zheng
   SPDX-License-Identifier: Apache-2.0

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
#include "Program.hpp"

namespace Piper {
    class JITCompiler : public Object {};

    class Function : public Object {};

    class IRBuilder;

    class ValueStorage {
    public:
        virtual ~ValueStorage() = default;
        virtual IRBuilder& getIRBuilder() noexcept = 0;
    };

#undef PIPER_ARITHMETIC_REDIRECT

    class IRBuilder : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(IRBuilder, Object);
        // Unary operator
        virtual ValueStorage* createNeg(ValueStorage* lhs) = 0;

        // Binary operator
        virtual ValueStorage* createAdd(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createSub(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createMul(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createDiv(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createMod(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createEqual(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createNotEqual(ValueStorage* lhs, ValueStorage* rhs) = 0;

        virtual ValueStorage* createGreaterThan(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createGreaterEqual(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createLessThan(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createLessEqual(ValueStorage* lhs, ValueStorage* rhs) = 0;

        // Logic
        virtual ValueStorage* createLogicNot(ValueStorage* val) = 0;
        // NOTICE: short-circuit evaluation
        virtual ValueStorage* createLogicAnd(ValueStorage* lhs, ValueStorage* rhs) = 0;
        virtual ValueStorage* createLogicOr(ValueStorage* lhs, ValueStorage* rhs) = 0;

        // Constant
        virtual ValueStorage* createConstant(float val) = 0;
        virtual ValueStorage* createConstant(double val) = 0;
        virtual ValueStorage* createConstant(uintmax_t val, uint32_t bits) = 0;

        // Arithmetic Cast
        virtual ValueStorage* createCast(ValueStorage* val, uint32_t bits) = 0;
    };

    class Bool {
    private:
        Variant<ValueStorage*, bool> mValue;

    public:
        explicit Bool(ValueStorage* val) noexcept : mValue{ val } {}
        Bool(bool val) noexcept : mValue{ val } {}
        Bool operator!() const {
            if(mValue.index())
                return Bool{ !eastl::get<1>(mValue) };
            const auto value = eastl::get<0>(mValue);
            return Bool{ value->getIRBuilder().createLogicNot(value) };
        }
    };

    template <typename T, typename = std::is_integral<T>>
    class Integer final {
    private:
        Variant<ValueStorage*, T> mValue;

    public:
        explicit Integer(ValueStorage* val) noexcept : mValue{ val } {}
        Integer(T val) noexcept : mValue{ val } {}

        template <typename U>
        explicit operator Integer<U>() const {
            if(mValue.index())
                return Integer<U>{ static_cast<U>(eastl::get<1>(mValue)) };
            const auto value = eastl::get<0>(mValue);
            return value->getIRBuilder().createCast(value, sizeof(U) * 8);
        }

        [[nodiscard]] ValueStorage* instantiate(IRBuilder& builder) const {
            return mValue.index() ? builder.createConstant(eastl::get<1>(mValue)) : eastl::get<0>(mValue);
        }
    };

    using Int8 = Integer<int8_t>;
    using Int16 = Integer<int16_t>;
    using Int32 = Integer<int32_t>;
    using Int64 = Integer<int64_t>;

    using UInt8 = Integer<uint8_t>;
    using UInt16 = Integer<uint16_t>;
    using UInt32 = Integer<uint32_t>;
    using UInt64 = Integer<uint64_t>;

    template <typename T, typename = std::is_floating_point<T>>
    class NativeFloatingPoint final {
    private:
        Variant<ValueStorage*, T> mValue;

    public:
        explicit NativeFloatingPoint(ValueStorage* val) noexcept : mValue{ val } {}
        NativeFloatingPoint(T val) noexcept : mValue{ val } {}

        template <typename U>
        explicit operator Integer<U>() const {
            if(mValue.index())
                return NativeFloatingPoint<U>{ static_cast<U>(eastl::get<1>(mValue)) };
            const auto value = eastl::get<0>(mValue);
            return value->getIRBuilder().createCast(value, sizeof(U) * 8);
        }

        [[nodiscard]] ValueStorage* instantiate(IRBuilder& builder) const {
            return mValue.index() ? builder.createConstant(eastl::get<1>(mValue)) : eastl::get<0>(mValue);
        }
    };

    using Float32 = NativeFloatingPoint<float>;
    using Float64 = NativeFloatingPoint<double>;

#define PIPER_BINARY_OPERATOR(TYPE, OP, INST)                                                                                    \
    template <typename U, typename V>                                                                                            \
    auto operator OP(TYPE<U> lhs, TYPE<V> rhs) {                                                                                 \
        using Result = decltype(std::declval<U>() OP std::declval<V>());                                                         \
        if(lhs.mValue.index() && rhs.mValue.index())                                                                             \
            return TYPE<Result>{ eastl::get<1>(lhs.mValue) OP eastl::get<1>(rhs.mValue) };                                       \
        auto&& builder = (lhs.mValue.index() ? eastl::get<0>(rhs.mValue) : eastl::get<0>(lhs.mValue))->getIRBuilder();           \
        return TYPE<Result>{ builder.INST(TYPE<Result>{ lhs }.instantiate(builder), TYPE<Result>{ rhs }.instantiate(builder)) }; \
    }

    PIPER_BINARY_OPERATOR(Integer, +, createAdd);
    PIPER_BINARY_OPERATOR(Integer, -, createSub);
    PIPER_BINARY_OPERATOR(Integer, *, createMul);
    PIPER_BINARY_OPERATOR(Integer, /, createDiv);
    PIPER_BINARY_OPERATOR(Integer, %, createMod);
    PIPER_BINARY_OPERATOR(Integer, >, createGreaterThan);
    PIPER_BINARY_OPERATOR(Integer, >=, createGreaterEqual);
    PIPER_BINARY_OPERATOR(Integer, <, createLessThan);
    PIPER_BINARY_OPERATOR(Integer, <=, createLessEqual);
    PIPER_BINARY_OPERATOR(Integer, ==, createEqual);
    PIPER_BINARY_OPERATOR(Integer, !=, createNotEqual);

    PIPER_BINARY_OPERATOR(NativeFloatingPoint, +, createAdd);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, -, createSub);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, *, createMul);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, /, createDiv);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, >, createGreaterThan);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, >=, createGreaterEqual);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, <, createLessThan);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, <=, createLessEqual);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, ==, createEqual);
    PIPER_BINARY_OPERATOR(NativeFloatingPoint, !=, createNotEqual);

#undef PIPER_BINARY_OPERATOR

#define PIPER_UNARY_OPERATOR(TYPE, OP, INST)                        \
    template <typename T>                                           \
    auto operator OP(TYPE<T> val) {                                 \
        if(val.mValue.index())                                      \
            return TYPE<T>{ OP eastl::get<1>(val.mValue) };         \
        auto&& builder = eastl::get<0>(val.mValue)->getIRBuilder(); \
        return TYPE<T>{ builder.INST(val.instantiate(builder)) };   \
    }

    PIPER_UNARY_OPERATOR(Integer, -, createNeg);
    PIPER_UNARY_OPERATOR(NativeFloatingPoint, -, createNeg);

#undef PIPER_UNARY_OPERATOR

    class FloatImpl {
    public:
        virtual ~FloatImpl();
        [[nodiscard]] virtual FloatImpl* add(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual FloatImpl* sub(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual FloatImpl* mul(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual FloatImpl* div(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual FloatImpl* neg(FloatImpl* rhs) const = 0;

        [[nodiscard]] virtual Bool greatThan(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual Bool lessThan(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual Bool greatEqual(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual Bool lessEqual(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual Bool equal(FloatImpl* rhs) const = 0;
        [[nodiscard]] virtual Bool notEqual(FloatImpl* rhs) const = 0;

        [[nodiscard]] virtual FloatImpl* sin() const = 0;
        [[nodiscard]] virtual FloatImpl* cos() const = 0;
        [[nodiscard]] virtual FloatImpl* tan() const = 0;
        [[nodiscard]] virtual FloatImpl* exp() const = 0;
        [[nodiscard]] virtual FloatImpl* abs() const = 0;
        [[nodiscard]] virtual FloatImpl* asin() const = 0;
        [[nodiscard]] virtual FloatImpl* acos() const = 0;
        [[nodiscard]] virtual FloatImpl* atan2(FloatImpl* rhs) const = 0;
    };

    class Float final {
    private:
        FloatImpl* mImpl;

    public:
        explicit Float(FloatImpl* impl) : mImpl{ impl } {}

#define PIPER_BINARY_OPERATOR(OP, INST)                      \
    [[nodiscard]] Float operator OP(const Float rhs) const { \
        return Float{ mImpl->INST(rhs.mImpl) };              \
    }

        PIPER_BINARY_OPERATOR(+, add);
        PIPER_BINARY_OPERATOR(-, sub);
        PIPER_BINARY_OPERATOR(*, mul);
        PIPER_BINARY_OPERATOR(/, div);

#undef PIPER_BINARY_OPERATOR

#define PIPER_BINARY_OPERATOR(OP, INST)                     \
    [[nodiscard]] Bool operator OP(const Float rhs) const { \
        return mImpl->INST(rhs.mImpl);                      \
    }

        PIPER_BINARY_OPERATOR(>, greatThan);
        PIPER_BINARY_OPERATOR(<, lessThan);
        PIPER_BINARY_OPERATOR(>=, greatEqual);
        PIPER_BINARY_OPERATOR(<=, lessEqual);
        PIPER_BINARY_OPERATOR(!=, notEqual);
        PIPER_BINARY_OPERATOR(==, equal);

#undef PIPER_BINARY_OPERATOR
    };

}  // namespace Piper
