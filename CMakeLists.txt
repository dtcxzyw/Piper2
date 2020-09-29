cmake_minimum_required (VERSION 3.8)
enable_testing()

project("Piper2")
set(CMAKE_CXX_STANDARD 17)
add_compile_definitions("UNICODE")
add_compile_definitions("_UNICODE")
add_compile_options("/utf-8")

add_subdirectory("Src")

install(FILES LICENSE DESTINATION ./)
