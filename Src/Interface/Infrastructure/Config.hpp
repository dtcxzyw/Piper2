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
#include "../../STL/GSL.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/String.hpp"
#include "../../STL/StringView.hpp"
#include "../../STL/UMap.hpp"
#include "../../STL/Variant.hpp"
#include "../Object.hpp"

namespace Piper {
    enum class NodeType { FloatingPoint, String, SignedInteger, UnsignedInteger, Boolean, Array, Object, Null };
    namespace Detail {
        template <typename T>
        struct DefaultTag;
    }
    class Config final : public Object {
    private:
        Variant<double, String, intmax_t, uintmax_t, bool, DynamicArray<SharedPtr<Config>>, UMap<String, SharedPtr<Config>>,
                MonoState>
            mValue;

    public:
        Config(PiperContext& context) : Object(context), mValue(MonoState{}) {}

        template <typename T, typename RT = std::decay_t<T>>
        Config(PiperContext& context, T&& value,
               Detail::DefaultTag<
                   std::enable_if_t<!(std::is_floating_point_v<RT> || (std::is_integral_v<RT> && !std::is_same_v<RT, bool>) ||
                                      std::is_same_v<RT, StringView> || std::is_same_v<RT, CString>)>>* unused = nullptr)
            : Object(context), mValue(std::forward<T>(value)) {}

        Config(PiperContext& context, const StringView& value) : Config(context, String{ value, context.getAllocator() }) {}

        Config(PiperContext& context, const CString value) : Config(context, String{ value, context.getAllocator() }) {}

        template <typename T, typename RT = std::decay_t<T>>
        Config(PiperContext& context, T value,
               Detail::DefaultTag<std::enable_if_t<std::is_floating_point_v<RT>>>* unused = nullptr)
            : Object(context), mValue(static_cast<double>(value)) {}

        template <typename T, typename RT = std::decay_t<T>>
        Config(
            PiperContext& context, T value,
            Detail::DefaultTag<std::enable_if_t<std::is_integral_v<RT> && std::is_unsigned_v<RT> && !std::is_same_v<RT, bool>>>*
                unused = nullptr)
            : Object(context), mValue(static_cast<uintmax_t>(value)) {}

        template <typename T, typename RT = std::decay_t<T>>
        Config(PiperContext& context, T value,
               Detail::DefaultTag<std::enable_if_t<std::is_integral_v<RT> && std::is_signed_v<RT>>>* unused = nullptr)
            : Object(context), mValue(static_cast<intmax_t>(value)) {}

        // TODO:reduce copy
        template <typename T>
        [[nodiscard]] T get() const {
            return Piper::get<T>(mValue);
        }

        // TODO:move to core
        [[nodiscard]] const UMap<String, SharedPtr<Config>>& viewAsObject() const {
            return Piper::get<UMap<String, SharedPtr<Config>>>(mValue);
        }

        [[nodiscard]] const DynamicArray<SharedPtr<Config>>& viewAsArray() const {
            return Piper::get<DynamicArray<SharedPtr<Config>>>(mValue);
        };

        [[nodiscard]] const SharedPtr<Config>& at(const StringView& key) const {
            return viewAsObject().find(String(key, context().getAllocator()))->second;
        }
        SharedPtr<Config>& at(const StringView& key) {
            if(type() == NodeType::Null)
                mValue = UMap<String, SharedPtr<Config>>{ context().getAllocator() };
            auto&& map = Piper::get<UMap<String, SharedPtr<Config>>>(mValue);
            auto& res = map[String(key, context().getAllocator())];
            if(!res)
                res = makeSharedObject<Config>(context());
            return res;
        }

        [[nodiscard]] NodeType type() const noexcept {
            return static_cast<NodeType>(mValue.index());
        }
    };

    class ConfigSerializer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ConfigSerializer, Object);
        virtual ~ConfigSerializer() = default;
        [[nodiscard]] virtual SharedPtr<Config> deserialize(const String& path) const = 0;
        virtual void serialize(const SharedPtr<Config>& config, const String& path) const = 0;
    };
}  // namespace Piper
