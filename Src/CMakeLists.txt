cmake_minimum_required (VERSION 3.8)

find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(EASTL CONFIG REQUIRED)
find_package(EABase CONFIG REQUIRED)
find_package(IlmBase CONFIG REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)

add_subdirectory("Infrastructure")
add_subdirectory("BuiltinComponent")
add_subdirectory("Test")

set_output_path(./)
add_library(PiperCore SHARED PiperCore.cpp PiperCoreWin32.cpp)
target_link_libraries(PiperCore PRIVATE EABase EASTL Microsoft.GSL::GSL)

set_output_path(./)
add_library(PiperRenderer SHARED PiperRenderer.cpp)
target_link_libraries(PiperRenderer PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore IlmBase::Iex IlmBase::Half IlmBase::Imath IlmBase::IexMath  OpenEXR::IlmImf OpenEXR::IlmImfUtil OpenEXR::IlmImfConfig)

#TODO:auto update
file(COPY Module.json DESTINATION  ${CMAKE_BINARY_DIR}/Bin)

set_output_path(./)
add_executable(PiperCLI PiperCLI.cpp)
target_link_libraries(PiperCLI PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
