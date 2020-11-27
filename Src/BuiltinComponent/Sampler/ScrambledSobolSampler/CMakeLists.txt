cmake_minimum_required (VERSION 3.8)

set_output_path(BuiltinComponent/Sampler/ScrambledSobolSampler)
add_library(ScrambledSobolSampler SHARED "ScrambledSobolSampler.cpp")
target_link_libraries(ScrambledSobolSampler PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore)

compile_kernel(ScrambledSobolSampler Kernel.cpp Kernel.bc)
