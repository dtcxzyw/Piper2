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
#include "../../STL/GSL.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/String.hpp"
#include "../../STL/StringView.hpp"
#include "../../STL/UMap.hpp"
#include "../../STL/Variant.hpp"
#include "../../STL/Vector.hpp"
#include "../Object.hpp"

namespace Piper {
    enum class NodeType { FloatingPoint, String, SignedInteger, UnsignedInteger, Boolean, Array, Object, Null };
    template <typename T>
    struct DefaultTag;
    class Config final : public Object {
    private:
        Variant<double, String, intmax_t, uintmax_t, bool, Vector<SharedObject<Config>>, UMap<String, SharedObject<Config>>,
                MonoState>
            mValue;

    public:
        Config(PiperContext& context) : Object(context), mValue(MonoState{}) {}

        template <typename T>
        Config(PiperContext& context, T&& value) : Object(context), mValue(value) {}

        void set(const StringView& value) {
            mValue = Piper::String(value, context().getAllocator());
        }

        void set(const char8_t* value) {
            mValue = Piper::String(value, context().getAllocator());
        }

        template <typename T>
        void
        set(T&& value,
            DefaultTag<std::enable_if_t<!(std::is_floating_point_v<T> || (std::is_integral_v<T> && !std::is_same_v<T, bool>) ||
                                          std::is_same_v<T, StringView> || std::is_same_v<std::remove_const_t<T>, char8_t>)>>*
                unused = nullptr) {
            mValue = std::forward<T>(value);
        }

        template <typename T>
        void set(T value, DefaultTag<std::enable_if_t<std::is_floating_point_v<T>>>* unused = nullptr) {
            mValue = static_cast<double>(value);
        }

        template <typename T>
        void
        set(T value,
            DefaultTag<std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T> && !std::is_same_v<T, bool>>>* unused =
                nullptr) {
            mValue = static_cast<uintmax_t>(value);
        }

        template <typename T>
        void set(T value, DefaultTag<std::enable_if_t<std::is_integral_v<T> && std::is_signed_v<T>>>* unused = nullptr) {
            mValue = static_cast<intmax_t>(value);
        }

        template <typename T>
        T get() const {
            return Piper::get<T>(mValue);
        }

        // TODO:move to core
        const UMap<String, SharedObject<Config>>& viewAsObject() const {
            return Piper::get<UMap<String, SharedObject<Config>>>(mValue);
        }

        const Vector<SharedObject<Config>>& viewAsArray() const {
            return Piper::get<Vector<SharedObject<Config>>>(mValue);
        };

        const Config& at(const StringView& key) const {
            return *(viewAsObject().find(String(key, context().getAllocator()))->second);
        }
        Config& at(const StringView& key) {
            if(type() == NodeType::Null)
                mValue = UMap<String, SharedObject<Config>>{ context().getAllocator() };
            auto&& map = Piper::get<UMap<String, SharedObject<Config>>>(mValue);
            auto& res = map[String(key, context().getAllocator())];
            if(!res)
                res = makeSharedObject<Config>(context());
            return *res;
        }

        NodeType type() const noexcept {
            return static_cast<NodeType>(mValue.index());
        }
    };

    class ConfigSerializer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ConfigSerializer, Object);
        virtual ~ConfigSerializer() = 0 {}
        virtual SharedObject<Config> deserialize(const StringView& path) const = 0;
        virtual void serialize(const SharedObject<Config>& config, const StringView& path) const = 0;
    };
}  // namespace Piper
