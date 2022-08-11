cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Texture/ProceduralTexture)
add_library(ProceduralTexture SHARED "ProceduralTexture.cpp")
target_link_libraries(ProceduralTexture PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)
