cmake_minimum_required (VERSION 3.8)

find_package(LLVM CONFIG REQUIRED)

set_output_path(Infrastructure/Program/)
add_library(LLVMIR SHARED "LLVMIR.cpp")
target_link_libraries(LLVMIR PRIVATE EABase EASTL Microsoft.GSL::GSL PiperCore LLVMCore LLVMBitReader LLVMLinker LLVMNVPTXInfo LLVMNVPTXCodeGen LLVMNVPTXDesc LLVMX86Info LLVMX86CodeGen LLVMX86Desc LLVMX86AsmParser)
target_include_directories(LLVMIR SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
#DEBUG
#target_compile_options(LLVMIR PRIVATE "/MDd")
target_compile_definitions(LLVMIR PRIVATE _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
