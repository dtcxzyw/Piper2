cmake_minimum_required (VERSION 3.8)

find_package(GTest CONFIG REQUIRED)

# function
add_executable(PiperCore-Test PiperCore-Test.cpp)
target_link_libraries(PiperCore-Test PRIVATE PiperCore GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
install(TARGETS PiperCore-Test RUNTIME DESTINATION ./)
#install(FILES gmock.dll gtest.dll gtest_main.dll DESTINATION ./)
add_test(NAME PiperCore-Test COMMAND ${CMAKE_INSTALL_PREFIX}/PiperCore-Test)
