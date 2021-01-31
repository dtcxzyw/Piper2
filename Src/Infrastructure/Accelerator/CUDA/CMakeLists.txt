cmake_minimum_required (VERSION 3.8)

include(FindCUDAToolkit)
set_output_path(Infrastructure/Accelerator/CUDAWrapper)
add_library(CUDAWrapper SHARED "CUDA.cpp")
target_link_libraries(CUDAWrapper PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore CUDA::cuda_driver)
target_include_directories(CUDAWrapper SYSTEM PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
