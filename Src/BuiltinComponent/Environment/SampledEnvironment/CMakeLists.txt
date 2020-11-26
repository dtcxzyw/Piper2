cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Environment/SampledEnvironment)
add_library(SampledEnvironment SHARED "SampledEnvironment.cpp")
target_link_libraries(SampledEnvironment PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

compile_kernel(SampledEnvironment Kernel.cpp Kernel.bc)
