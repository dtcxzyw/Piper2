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

extern "C" {

struct Float {
    char first;
    char second[3];
};

inline static Float from(float val) {
    return *reinterpret_cast<Float*>(&val);
}

inline static float to(const Float& val) {
    return *reinterpret_cast<const float*>(&val);
}

void add(Float& res, const Float& a, const Float& b) {
    res = from(to(a) + to(b));
}

void sub(Float& res, const Float& a, const Float& b) {
    res = from(to(a) - to(b));
}

void mul(Float& res, const Float& a, const Float& b) {
    res = from(to(a) * to(b));
}

void div(Float& res, const Float& a, const Float& b) {
    res = from(to(a) / to(b));
}

void fma(Float& res, const Float& x, const Float& y, const Float& z) {
    res = from(to(x) * to(y) + to(z));
}

void constant(Float& res, double val) {
    res = from(static_cast<float>(val));
}

bool less(const Float& x, const Float& y) {
    return to(x) < to(y);
}

bool greater(const Float& x, const Float& y) {
    return to(x) > to(y);
}
}
