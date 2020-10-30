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

#ifdef PIPER_FP_NATIVE
#define PIPER_FP_TYPE(Float)
#else
#define PIPER_FP_TYPE(Float)                       \
    extern "C" {                                   \
    struct Float##_PIPER_FP final {                \
        double placeholder[4];                     \
    };                                             \
    typedef Float##_PIPER_FP Float;                \
    Float add##Float(Float lhs, Float rhs);        \
    Float sub##Float(Float lhs, Float rhs);        \
    Float mul##Float(Float lhs, Float rhs);        \
    Float div##Float(Float lhs, Float rhs);        \
    Float constant##Float(double val);             \
    Float fma##Float(Float x, Float y, Float z);   \
    bool less##Float(Float x, Float y);            \
    bool greater##Float(Float x, Float y);          \
    }                                              \
    inline Float operator+(Float lhs, Float rhs) { \
        return add##Float(lhs, rhs);               \
    }                                              \
    inline Float operator-(Float lhs, Float rhs) { \
        return sub##Float(lhs, rhs);               \
    }                                              \
    inline Float operator*(Float lhs, Float rhs) { \
        return mul##Float(lhs, rhs);               \
    }                                              \
    inline Float operator/(Float lhs, Float rhs) { \
        return mul##Float(lhs, rhs);               \
    }                                              \
    inline bool operator<(Float lhs, Float rhs) { \
        return less##Float(lhs, rhs);              \
    }                                              \
    inline bool operator>(Float lhs, Float rhs) { \
        return greater##Float(lhs, rhs);           \
    }
#endif
