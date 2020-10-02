cmake_minimum_required (VERSION 3.8)

find_library(JEMALLOC_LIB jemalloc)

set_output_path(Infrastructure/Allocator/)
add_library(JemallocAllocator SHARED "JemallocAllocator.cpp")
target_link_libraries(JemallocAllocator PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore ${JEMALLOC_LIB})
