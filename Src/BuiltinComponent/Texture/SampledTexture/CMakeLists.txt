cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Texture/SampledTexture)
add_library(SampledTexture SHARED "SampledTexture.cpp")
target_link_libraries(SampledTexture PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

compile_kernel(SampledTexture Kernel.cpp Kernel.bc)
