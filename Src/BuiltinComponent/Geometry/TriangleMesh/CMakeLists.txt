cmake_minimum_required (VERSION 3.8)

find_package(Assimp CONFIG REQUIRED)
set_output_path(BuiltinComponent/Geometry/TriangleMesh)
add_library(TriangleMesh SHARED "TriangleMesh.cpp")
target_link_libraries(TriangleMesh PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore assimp::assimp)
