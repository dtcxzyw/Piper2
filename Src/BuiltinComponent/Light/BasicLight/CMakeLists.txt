cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Light/BasicLight)
add_library(BasicLight SHARED "BasicLight.cpp")
target_link_libraries(BasicLight PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

compile_kernel(BasicLight Kernel.cpp Kernel.bc)
