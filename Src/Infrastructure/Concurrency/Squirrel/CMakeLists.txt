cmake_minimum_required (VERSION 3.8)

set_output_path(Infrastructure/Concurrency/Squirrel)
add_library(Squirrel SHARED "Squirrel.cpp")
target_link_libraries(Squirrel PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
