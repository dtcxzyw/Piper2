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
#include "../../STL/GSL.hpp"
#include "../../STL/Pair.hpp"
#include "../../STL/String.hpp"
#include "../../STL/StringView.hpp"
#include "../../STL/UMap.hpp"
#include "../../STL/Variant.hpp"
#include "../../STL/Vector.hpp"
#include "../Object.hpp"

namespace Piper {
    class PIPER_API Config final : public Object {
    private:
        Variant<double, String, int64_t, uint64_t, bool, Vector<SharedObject<Config>>, UMap<String, SharedObject<Config>>,
                MonoState>
            mValue;

    public:
        PIPER_INTERFACE_CONSTRUCT(Config, Object);
        template <typename T>
        void set(T&& value) {
            mValue = std::forward<T>(value);
        }

        template <typename T>
        T get() const {
            return Piper::get<T>(mValue);
        }

        const UMap<String, SharedObject<Config>>& viewAsObject() const;

        const Vector<SharedObject<Config>>& viewAsArray() const;

        const SharedObject<Config>& operator()(const StringView& key) const;
        const SharedObject<Config>& operator[](Index index) const;
    };

    class ConfigSerializer : public Object {
    public:
        PIPER_INTERFACE_CONSTRUCT(ConfigSerializer, Object);
        virtual ~ConfigSerializer() = 0 {}
        virtual SharedObject<Config> deserialize(const StringView& path) const = 0;
        virtual void serialize(const SharedObject<Config>& config, const StringView& path) const = 0;
    };
}  // namespace Piper
