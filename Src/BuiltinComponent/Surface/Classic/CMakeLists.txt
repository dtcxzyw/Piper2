cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Surface/Classic)
add_library(Classic SHARED "Classic.cpp")
target_link_libraries(Classic PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
