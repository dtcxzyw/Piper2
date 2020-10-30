cmake_minimum_required (VERSION 3.8)

find_package(LLVM CONFIG REQUIRED)

set_output_path(Infrastructure/Accelerator)
add_library(Parallel SHARED "Parallel.cpp")
target_link_libraries(Parallel PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore LLVMCore LLVMX86Info LLVMX86CodeGen LLVMX86Desc LLVMOrcJIT LLVMLinker)
target_include_directories(Parallel SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
#DEBUG
#target_compile_options(Parallel PRIVATE "/MD")
target_compile_definitions(Parallel PRIVATE _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
