cmake_minimum_required (VERSION 3.8)

find_package(GTest CONFIG REQUIRED)

# function
set_output_path(./)
add_executable(PiperCore-Test PiperCore-Test.cpp)
target_link_libraries(PiperCore-Test PRIVATE PiperCore GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main EASTL)
add_test(NAME PiperCore-Test COMMAND PiperCore-Test)
