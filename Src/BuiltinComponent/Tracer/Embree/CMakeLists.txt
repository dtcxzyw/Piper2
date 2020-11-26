cmake_minimum_required (VERSION 3.8)

find_package(embree CONFIG 3.12.1 REQUIRED)

set_output_path(BuiltinComponent/Tracer/Embree)
add_library(EmbreeWrapper SHARED "Embree.cpp")
target_link_libraries(EmbreeWrapper PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore embree)
