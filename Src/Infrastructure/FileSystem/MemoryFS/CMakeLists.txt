cmake_minimum_required (VERSION 3.8)

set_output_path(Infrastructure/FileSystem/)
add_library(MemoryFS SHARED "MemoryFS.cpp")
target_link_libraries(MemoryFS PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
