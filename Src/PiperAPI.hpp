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
#define PIPER_CC _stdcall

#ifndef PIPER_API
#if defined(_MSC_VER)
#ifdef PIPER_EXPORT
#define PIPER_API __declspec(dllexport)
#else
#define PIPER_API __declspec(dllimport)
#endif
#else
#define PIPER_API
#endif
#endif

#define PIPER_INTERFACE "0"

#if defined(_MSC_VER)
#define PIPER_ABI "MSVC"
#else
#error "Unsupported ABI"
#endif

#define PIPER_STL "EASTL"
