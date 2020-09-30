cmake_minimum_required (VERSION 3.8)

set_output_path(Infrastructure/FileSystem/)
add_library(MemoryFile SHARED MemoryFile.cpp)
target_link_libraries(MemoryFile PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
