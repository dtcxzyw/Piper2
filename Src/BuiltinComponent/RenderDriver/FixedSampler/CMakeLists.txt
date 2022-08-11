cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/RenderDriver/FixedSampler)
add_library(FixedSampler SHARED "FixedSampler.cpp")
target_link_libraries(FixedSampler PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
