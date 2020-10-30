cmake_minimum_required (VERSION 3.8)

set_output_path(Infrastructure/PerformancePrimitivesLibrary)
add_library(Builtin SHARED "Builtin.cpp")
target_link_libraries(Builtin PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

add_custom_command(TARGET Builtin PRE_BUILD COMMAND clang -emit-llvm Kernel.cpp -c -O3 -o ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Kernel.bc WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
