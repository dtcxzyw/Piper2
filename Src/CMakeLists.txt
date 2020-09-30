cmake_minimum_required (VERSION 3.8)

find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(EASTL CONFIG REQUIRED)
find_package(EABase CONFIG REQUIRED)

add_subdirectory("Infrastructure")
add_subdirectory("BuiltinComponent")
add_subdirectory("Test")

set_output_path(./)
add_library(PiperCore SHARED PiperCore.cpp )
target_link_libraries(PiperCore PRIVATE EABase EASTL Microsoft.GSL::GSL)

set_output_path(./)
add_executable(PiperCLI PiperCLI.cpp )
target_link_libraries(PiperCLI PRIVATE EABase EASTL Microsoft.GSL::GSL)
