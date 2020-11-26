cmake_minimum_required (VERSION 3.8)

set_output_path(Infrastructure/Concurrency/Taskflow)
add_library(Taskflow SHARED "Taskflow.cpp")
target_link_libraries(Taskflow PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
