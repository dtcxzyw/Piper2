cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Sensor/PerspectiveCamera)
add_library(PerspectiveCamera SHARED "PerspectiveCamera.cpp")
target_link_libraries(PerspectiveCamera PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
