cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Light/AreaLight)
add_library(AreaLight SHARED "AreaLight.cpp")
target_link_libraries(AreaLight PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
