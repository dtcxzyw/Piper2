cmake_minimum_required (VERSION 3.8)

find_package(GTest CONFIG REQUIRED)

#TODO separate from Bin
set_output_path(./)

function(add_piper_test name)
add_executable(${name}-Test ${name}-Test.cpp)
target_link_libraries(${name}-Test PRIVATE PiperCore GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main EASTL)
add_test(NAME ${name}-Test COMMAND ${name}-Test)
endfunction()

add_piper_test(PiperCore)
add_piper_test(Allocator)
add_piper_test(Concurrency)
add_piper_test(Accelerator)
add_piper_test(Sampler)

compile_kernel(Accelerator-Test conv.cpp conv.bc)
