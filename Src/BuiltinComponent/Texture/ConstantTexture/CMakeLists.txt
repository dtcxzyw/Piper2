cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Texture/ConstantTexel)
add_library(ConstantTexture SHARED "ConstantTexture.cpp")
target_link_libraries(ConstantTexture PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

compile_kernel(ConstantTexture Kernel.cpp Kernel.bc)
