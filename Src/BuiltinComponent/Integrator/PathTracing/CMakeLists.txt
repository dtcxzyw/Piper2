cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Integrator/PathTracing)
add_library(PathTracing SHARED "PathTracing.cpp")
target_link_libraries(PathTracing PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
