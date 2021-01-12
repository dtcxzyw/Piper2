cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Geometry/BasicShapes)
add_library(BasicShapes SHARED "BasicShapes.cpp")
target_link_libraries(BasicShapes PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

compile_kernel(BasicShapes Kernel.cpp Kernel.bc)
