cmake_minimum_required (VERSION 3.8)

include(FindCUDAToolkit)
set_output_path(Infrastructure/Accelerator/CUDAWrapper)
add_library(CUDAWrapper SHARED "CUDA.cpp" "Kernel.cpp")
target_link_libraries(CUDAWrapper PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore CUDA::cuda_driver)
target_include_directories(CUDAWrapper SYSTEM PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
#TODO:auto update
file(COPY ${CUDAToolkit_TARGET_DIR}/nvvm/libdevice/libdevice.10.bc DESTINATION  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/libdevice.10.bc )

compile_kernel(CUDAWrapper Kernel.cpp Kernel.bc)
