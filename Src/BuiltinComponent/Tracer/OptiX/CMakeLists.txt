cmake_minimum_required (VERSION 3.8)

include(FindCUDAToolkit)

set_output_path(BuiltinComponent/Tracer/OptiX)
add_library(OptiX SHARED "OptiX.cpp")
target_include_directories(OptiX PUBLIC "$ENV{OPTIX_PATH}/include")
target_include_directories(OptiX SYSTEM PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
target_link_libraries(OptiX PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore CUDA::cuda_driver)
